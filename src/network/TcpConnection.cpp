#include "TcpConnection.h"
#include "LedgerFrameCodec.h"
#include "platform/NetworkPlatform.h"

#include <cstring>

namespace pp {
namespace network {

namespace {

TcpConnection::Roe<void> recvExact(SocketHandle fd, void* out, size_t len) {
  auto* p = static_cast<uint8_t*>(out);
  size_t off = 0;
  while (off < len) {
#if defined(_WIN32)
    const int n = ::recv(static_cast<SOCKET>(fd), reinterpret_cast<char*>(p + off),
                         static_cast<int>(len - off), 0);
#else
    const ssize_t n = ::recv(fd, p + off, len - off, 0);
#endif
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      return TcpConnection::Error("Connection closed by peer");
    }
    const int err = socketLastError();
    if (socketInterrupted(err)) {
      continue;
    }
    if (socketWouldBlock(err)) {
      return TcpConnection::Error("Receive timeout (no data within socket timeout)");
    }
    return TcpConnection::Error("Failed to receive data: " + socketErrorString(err));
  }
  return {};
}

TcpConnection::Roe<void> sendAll(SocketHandle fd, const void* data, size_t len) {
  auto* p = static_cast<const uint8_t*>(data);
  size_t off = 0;
  while (off < len) {
#if defined(_WIN32)
    const int n = ::send(static_cast<SOCKET>(fd), reinterpret_cast<const char*>(p + off),
                         static_cast<int>(len - off), 0);
#else
    const ssize_t n = ::send(fd, p + off, len - off, 0);
#endif
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      return TcpConnection::Error("Failed to send data");
    }
    const int err = socketLastError();
    if (socketInterrupted(err)) {
      continue;
    }
    if (socketWouldBlock(err)) {
      return TcpConnection::Error("Send timeout (no progress within socket timeout)");
    }
    return TcpConnection::Error("Failed to send data: " + socketErrorString(err));
  }
  return {};
}

} // namespace

TcpConnection::TcpConnection(int socket_fd)
    : socketFd_(socket_fd) {
  struct sockaddr_in peer_addr {};
  socklen_t addr_len = sizeof(peer_addr);
  if (getpeername(socketFd_, reinterpret_cast<struct sockaddr*>(&peer_addr),
                  &addr_len) == 0) {
    char addr_str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &peer_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
    peer_.address = addr_str;
    peer_.port = ntohs(peer_addr.sin_port);
  }
}

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : socketFd_(other.socketFd_), peer_(std::move(other.peer_)) {
  other.socketFd_ = kInvalidSocket;
  other.peer_ = {};
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this != &other) {
    close();
    socketFd_ = other.socketFd_;
    peer_ = std::move(other.peer_);
    other.socketFd_ = kInvalidSocket;
    other.peer_ = {};
  }
  return *this;
}

TcpConnection::Roe<size_t> TcpConnection::send(const void* data, size_t length) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

#if defined(_WIN32)
  const int sent = ::send(static_cast<SOCKET>(socketFd_), static_cast<const char*>(data),
                          static_cast<int>(length), 0);
#else
  const ssize_t sent = ::send(socketFd_, data, length, 0);
#endif
  if (sent < 0) {
    return Error("Failed to send data");
  }

  return Roe<size_t>(static_cast<size_t>(sent));
}

TcpConnection::Roe<size_t> TcpConnection::send(const std::string& message) {
  return send(message.c_str(), message.length());
}

TcpConnection::Roe<size_t> TcpConnection::sendAndShutdown(const void* data, size_t length) {
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

TcpConnection::Roe<size_t> TcpConnection::sendAndShutdown(const std::string& message) {
  return sendAndShutdown(message.c_str(), message.length());
}

TcpConnection::Roe<void> TcpConnection::shutdownWrite() {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

#if defined(_WIN32)
  if (shutdown(static_cast<SOCKET>(socketFd_), SD_SEND) == SOCKET_ERROR) {
#else
  if (shutdown(socketFd_, SHUT_WR) < 0) {
#endif
    return Error("Failed to shutdown write: " +
                 socketErrorString(socketLastError()));
  }

  return {};
}

TcpConnection::Roe<size_t> TcpConnection::receive(void* buffer, size_t maxLength) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

#if defined(_WIN32)
  const int received =
      ::recv(static_cast<SOCKET>(socketFd_), static_cast<char*>(buffer),
             static_cast<int>(maxLength), 0);
#else
  const ssize_t received = recv(socketFd_, buffer, maxLength, 0);
#endif
  if (received < 0) {
    const int err = socketLastError();
    if (socketWouldBlock(err)) {
      return Error("Receive timeout (no data within socket timeout)");
    }
    return Error("Failed to receive data: " + socketErrorString(err));
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

  auto lenResult = LedgerFrameCodec::decodeLengthPrefix(&netLen, sizeof(netLen));
  if (!lenResult) {
    return Error(lenResult.error().message);
  }
  const uint32_t len = lenResult.value();

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

  auto framed = LedgerFrameCodec::encode(body);
  if (!framed) {
    return Error(framed.error().message);
  }

  auto h = sendAll(socketFd_, framed.value().data(), framed.value().size());
  if (!h) {
    return Error(h.error().message);
  }
  return {};
}

TcpConnection::Roe<void> TcpConnection::setTimeout(std::chrono::milliseconds timeout) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }
  if (!socketSetTimeout(socketFd_, timeout)) {
    return Error("Failed to set socket timeout: " +
                 socketErrorString(socketLastError()));
  }
  return {};
}

void TcpConnection::close() {
  if (socketFd_ >= 0) {
    socketClose(socketFd_);
    socketFd_ = kInvalidSocket;
  }
}

const IpEndpoint& TcpConnection::getPeerEndpoint() const { return peer_; }

} // namespace network
} // namespace pp
