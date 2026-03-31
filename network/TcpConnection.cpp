#include "TcpConnection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace pp {
namespace network {

namespace {

TcpConnection::Roe<void> recvExact(int fd, void* out, size_t len) {
  auto* p = static_cast<uint8_t*>(out);
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::recv(fd, p + off, len - off, 0);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      return TcpConnection::Error("Connection closed by peer");
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return TcpConnection::Error("Receive timeout (no data within socket timeout)");
    }
    return TcpConnection::Error("Failed to receive data: " + std::string(std::strerror(errno)));
  }
  return {};
}

TcpConnection::Roe<void> sendAll(int fd, const void* data, size_t len) {
  auto* p = static_cast<const uint8_t*>(data);
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, p + off, len - off, 0);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      return TcpConnection::Error("Failed to send data");
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return TcpConnection::Error("Send timeout (no progress within socket timeout)");
    }
    return TcpConnection::Error("Failed to send data: " + std::string(std::strerror(errno)));
  }
  return {};
}

} // namespace

TcpConnection::TcpConnection(int socket_fd)
    : socketFd_(socket_fd) {
  // Get peer address information
  struct sockaddr_in peer_addr;
  socklen_t addr_len = sizeof(peer_addr);
  if (getpeername(socketFd_, (struct sockaddr *)&peer_addr, &addr_len) == 0) {
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
    peer_.address = addr_str;
    peer_.port = ntohs(peer_addr.sin_port);
  }
}

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection &&other) noexcept
    : socketFd_(other.socketFd_), peer_(std::move(other.peer_)) {
  other.socketFd_ = -1;
  other.peer_ = {};
}

TcpConnection &TcpConnection::operator=(TcpConnection &&other) noexcept {
  if (this != &other) {
    close();
    socketFd_ = other.socketFd_;
    peer_ = std::move(other.peer_);
    other.socketFd_ = -1;
    other.peer_ = {};
  }
  return *this;
}

TcpConnection::Roe<size_t> TcpConnection::send(const void *data,
                                               size_t length) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

  ssize_t sent = ::send(socketFd_, data, length, 0);
  if (sent < 0) {
    return Error("Failed to send data");
  }

  return Roe<size_t>(static_cast<size_t>(sent));
}

TcpConnection::Roe<size_t> TcpConnection::send(const std::string &message) {
  return send(message.c_str(), message.length());
}

TcpConnection::Roe<size_t> TcpConnection::sendAndShutdown(const void *data,
                                                           size_t length) {
  auto result = send(data, length);
  if (!result) {
    return result;
  }
  
  auto shutdownResult = shutdownWrite();
  if (!shutdownResult) {
    return Error("Failed to shutdown write: " + shutdownResult.error().message);
  }
  
  return result;
}

TcpConnection::Roe<size_t> TcpConnection::sendAndShutdown(const std::string &message) {
  return sendAndShutdown(message.c_str(), message.length());
}

TcpConnection::Roe<void> TcpConnection::shutdownWrite() {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

  if (shutdown(socketFd_, SHUT_WR) < 0) {
    return Error("Failed to shutdown write: " + std::string(std::strerror(errno)));
  }

  return {};
}

TcpConnection::Roe<size_t> TcpConnection::receive(void *buffer,
                                                  size_t maxLength) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

  ssize_t received = recv(socketFd_, buffer, maxLength, 0);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return Error("Receive timeout (no data within socket timeout)");
    }
    return Error("Failed to receive data: " + std::string(std::strerror(errno)));
  }
  if (received == 0) {
    return Error("Connection closed by peer");
  }

  return Roe<size_t>(static_cast<size_t>(received));
}

TcpConnection::Roe<std::string> TcpConnection::receiveLine() {
  std::string line;
  char ch = 0;

  while (true) {
    auto result = receive(&ch, 1);
    if (result.isError()) {
      return Error(result.error().message);
    }

    if (ch == '\n') {
      break;
    }
    if (ch != '\r') {
      line += ch;
    }
  }

  return Roe<std::string>(line);
}

TcpConnection::Roe<std::string> TcpConnection::readFrame(std::chrono::milliseconds timeout) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

  // Ensure socket timeout is applied consistently for this operation.
  if (timeout.count() > 0) {
    auto t = setTimeout(timeout);
    if (!t) {
      return Error(t.error().message);
    }
  }

  uint32_t netLen = 0;
  auto hdr = recvExact(socketFd_, &netLen, sizeof(netLen));
  if (!hdr) {
    return Error(hdr.error().message);
  }

  uint32_t len = ntohl(netLen);
  if (len > MAX_FRAME_SIZE) {
    return Error("Frame too large: " + std::to_string(len));
  }

  std::string body;
  body.resize(len);
  if (len > 0) {
    auto b = recvExact(socketFd_, body.data(), len);
    if (!b) {
      return Error(b.error().message);
    }
  }

  return body;
}

TcpConnection::Roe<void> TcpConnection::writeFrame(std::string_view body) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }
  if (body.size() > MAX_FRAME_SIZE) {
    return Error("Frame too large: " + std::to_string(body.size()));
  }

  uint32_t netLen = htonl(static_cast<uint32_t>(body.size()));
  auto h = sendAll(socketFd_, &netLen, sizeof(netLen));
  if (!h) {
    return Error(h.error().message);
  }
  if (!body.empty()) {
    auto b = sendAll(socketFd_, body.data(), body.size());
    if (!b) {
      return Error(b.error().message);
    }
  }
  return {};
}

TcpConnection::Roe<void> TcpConnection::setTimeout(std::chrono::milliseconds timeout) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

  struct timeval tv;
  tv.tv_sec = timeout.count() / 1000;
  tv.tv_usec = (timeout.count() % 1000) * 1000;

  if (setsockopt(socketFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    return Error("Failed to set receive timeout: " + std::string(std::strerror(errno)));
  }
  if (setsockopt(socketFd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
    return Error("Failed to set send timeout: " + std::string(std::strerror(errno)));
  }

  return {};
}

void TcpConnection::close() {
  if (socketFd_ >= 0) {
    ::close(socketFd_);
    socketFd_ = -1;
  }
}

const IpEndpoint &TcpConnection::getPeerEndpoint() const { return peer_; }

} // namespace network
} // namespace pp
