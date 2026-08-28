#include "TcpClient.h"
#include "platform/NetworkPlatform.h"

#include <cstring>

namespace pp {
namespace network {

TcpClient::TcpClient() = default;

TcpClient::~TcpClient() { close(); }

TcpClient::TcpClient(TcpClient&& other) noexcept
    : connection_(std::move(other.connection_)) {
  other.connection_.reset();
}

TcpClient& TcpClient::operator=(TcpClient&& other) noexcept {
  if (this != &other) {
    close();
    connection_ = std::move(other.connection_);
    other.connection_.reset();
  }
  return *this;
}

TcpClient::Roe<void> TcpClient::connect(const IpEndpoint& endpoint) {
  if (connection_.has_value()) {
    return Error("Already connected");
  }
  if (!networkPlatformInit()) {
    return Error("Failed to initialize network platform");
  }

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* result = nullptr;
  const int gai = getaddrinfo(endpoint.address.c_str(),
                              std::to_string(endpoint.port).c_str(), &hints,
                              &result);
  if (gai != 0 || result == nullptr) {
    return Error("Failed to resolve hostname: " + endpoint.address);
  }

  SocketHandle socket_fd = kInvalidSocket;
  for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    socket_fd = static_cast<SocketHandle>(
        ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
    if (socket_fd < 0) {
      continue;
    }
    if (::connect(socket_fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
      break;
    }
    socketClose(socket_fd);
    socket_fd = kInvalidSocket;
  }

  freeaddrinfo(result);

  if (socket_fd < 0) {
    return Error("Failed to connect to " + endpoint.address + ":" +
                 std::to_string(endpoint.port));
  }

  connection_ = TcpConnection(socket_fd);
  return {};
}

TcpClient::Roe<size_t> TcpClient::send(const void* data, size_t length) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->send(data, length);
  if (result.isError()) {
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<size_t> TcpClient::send(const std::string& message) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->send(message);
  if (result.isError()) {
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<size_t> TcpClient::sendAndShutdown(const void* data, size_t length) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->sendAndShutdown(data, length);
  if (result.isError()) {
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<size_t> TcpClient::sendAndShutdown(const std::string& message) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->sendAndShutdown(message);
  if (result.isError()) {
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<void> TcpClient::shutdownWrite() {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->shutdownWrite();
  if (result.isError()) {
    return Error(result.error().message);
  }
  return {};
}

TcpClient::Roe<void> TcpClient::setTimeout(std::chrono::milliseconds timeout) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->setTimeout(timeout);
  if (result.isError()) {
    return Error(result.error().message);
  }
  return {};
}

TcpClient::Roe<size_t> TcpClient::receive(void* buffer, size_t maxLength) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->receive(buffer, maxLength);
  if (result.isError()) {
    if (result.error().message.find("closed") != std::string::npos) {
      connection_.reset();
    }
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<std::string> TcpClient::receiveLine() {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->receiveLine();
  if (result.isError()) {
    return Error(result.error().message);
  }
  return Roe<std::string>(result.value());
}

TcpClient::Roe<void> TcpClient::writeFrame(std::string_view body) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }
  auto r = connection_->writeFrame(body);
  if (!r) {
    return Error(r.error().message);
  }
  return {};
}

TcpClient::Roe<std::string> TcpClient::readFrame(std::chrono::milliseconds timeout) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }
  auto r = connection_->readFrame(timeout);
  if (!r) {
    if (r.error().message.find("closed") != std::string::npos) {
      connection_.reset();
    }
    return Error(r.error().message);
  }
  return Roe<std::string>(r.value());
}

void TcpClient::close() {
  if (connection_.has_value()) {
    connection_->close();
    connection_.reset();
  }
}

bool TcpClient::isConnected() const { return connection_.has_value(); }

} // namespace network
} // namespace pp
