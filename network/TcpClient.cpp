#include "TcpClient.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pp {
namespace network {

TcpClient::TcpClient() = default;

TcpClient::~TcpClient() { close(); }

TcpClient::TcpClient(TcpClient &&other) noexcept
    : connection_(std::move(other.connection_)) {
  other.connection_.reset();
}

TcpClient &TcpClient::operator=(TcpClient &&other) noexcept {
  if (this != &other) {
    close();
    connection_ = std::move(other.connection_);
    other.connection_.reset();
  }
  return *this;
}

TcpClient::Roe<void> TcpClient::connect(const TcpEndpoint &endpoint) {
  if (connection_.has_value()) {
    return Error("Already connected");
  }

  // Create socket
  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    return Error("Failed to create socket");
  }

  // Resolve hostname
  struct hostent *server = gethostbyname(endpoint.address.c_str());
  if (server == nullptr) {
    ::close(socket_fd);
    return Error("Failed to resolve hostname: " + endpoint.address);
  }

  // Setup address structure
  struct sockaddr_in server_addr;
  std::memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  std::memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
  server_addr.sin_port = htons(endpoint.port);

  // Connect to server
  if (::connect(socket_fd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
    ::close(socket_fd);
    return Error("Failed to connect to " + endpoint.address + ":" +
                 std::to_string(endpoint.port));
  }

  // Create TcpConnection from the connected socket
  connection_ = TcpConnection(socket_fd);
  return {};
}

TcpClient::Roe<size_t> TcpClient::send(const void *data, size_t length) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->send(data, length);
  if (result.isError()) {
    // Convert TcpConnection::Error to TcpClient::Error
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<size_t> TcpClient::send(const std::string &message) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->send(message);
  if (result.isError()) {
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<size_t> TcpClient::sendAndShutdown(const void *data,
                                                   size_t length) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->sendAndShutdown(data, length);
  if (result.isError()) {
    return Error(result.error().message);
  }
  return Roe<size_t>(result.value());
}

TcpClient::Roe<size_t> TcpClient::sendAndShutdown(const std::string &message) {
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

TcpClient::Roe<size_t> TcpClient::receive(void *buffer, size_t maxLength) {
  if (!connection_.has_value()) {
    return Error("Not connected");
  }

  auto result = connection_->receive(buffer, maxLength);
  if (result.isError()) {
    // If connection closed, clear the connection
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

void TcpClient::close() {
  if (connection_.has_value()) {
    connection_->close();
    connection_.reset();
  }
}

bool TcpClient::isConnected() const { return connection_.has_value(); }

} // namespace network
} // namespace pp
