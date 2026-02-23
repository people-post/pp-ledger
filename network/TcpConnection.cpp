#include "TcpConnection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace pp {
namespace network {

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
  char ch;

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

const TcpEndpoint &TcpConnection::getPeerEndpoint() const { return peer_; }

} // namespace network
} // namespace pp
