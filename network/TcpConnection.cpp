#include "TcpConnection.h"

#include <arpa/inet.h>
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

TcpConnection::Roe<size_t> TcpConnection::receive(void *buffer,
                                                  size_t maxLength) {
  if (socketFd_ < 0) {
    return Error("Connection closed");
  }

  ssize_t received = recv(socketFd_, buffer, maxLength, 0);
  if (received < 0) {
    return Error("Failed to receive data");
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

void TcpConnection::close() {
  if (socketFd_ >= 0) {
    ::close(socketFd_);
    socketFd_ = -1;
  }
}

const TcpEndpoint &TcpConnection::getPeerEndpoint() const { return peer_; }

} // namespace network
} // namespace pp
