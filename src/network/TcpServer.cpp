#include "TcpServer.h"
#include "platform/HostAddress.h"
#include "platform/IoMultiplexer.h"
#include "platform/NetworkPlatform.h"

#include <cstring>
#include <string>

namespace pp {
namespace network {

TcpServer::TcpServer() = default;

TcpServer::~TcpServer() { stop(); }

TcpServer::Roe<void> TcpServer::listen(const IpEndpoint& endpoint, int backlog) {
  if (listening_) {
    return Error("Server already listening");
  }
  if (!networkPlatformInit()) {
    return Error("Failed to initialize network platform");
  }

  socketFd_ = static_cast<SocketHandle>(::socket(AF_INET, SOCK_STREAM, 0));
  if (socketFd_ < 0) {
    return Error("Failed to create socket");
  }

  // Windows SO_REUSEADDR allows multiple listeners on the same port; use
  // SO_EXCLUSIVEADDRUSE so a second bind fails (matches POSIX SO_REUSEADDR).
#if defined(_WIN32)
  BOOL exclusive = TRUE;
  if (setsockopt(socketFd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) < 0) {
#else
  int opt = 1;
  if (setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
#endif
    socketClose(socketFd_);
    socketFd_ = kInvalidSocket;
    return Error("Failed to set socket options");
  }

  sockaddr_in server_addr {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(endpoint.port);
  endpoint_.address = endpoint.address;

  if (endpoint.address == "0.0.0.0" || endpoint.address.empty()) {
    server_addr.sin_addr.s_addr = INADDR_ANY;
  } else if (endpoint.address == "localhost" || endpoint.address == "127.0.0.1") {
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) != 1) {
      socketClose(socketFd_);
      socketFd_ = kInvalidSocket;
      return Error("Failed to resolve localhost address");
    }
  } else {
    if (inet_pton(AF_INET, endpoint.address.c_str(), &server_addr.sin_addr) != 1) {
      socketClose(socketFd_);
      socketFd_ = kInvalidSocket;
      return Error("Invalid host address: " + endpoint.address);
    }
  }

  if (bind(socketFd_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
    socketClose(socketFd_);
    socketFd_ = kInvalidSocket;
    return Error("Failed to bind to port " + std::to_string(endpoint.port));
  }

  if (::listen(socketFd_, backlog) < 0) {
    socketClose(socketFd_);
    socketFd_ = kInvalidSocket;
    return Error("Failed to listen on port " + std::to_string(endpoint.port));
  }

  if (!socketSetNonBlocking(socketFd_)) {
    socketClose(socketFd_);
    socketFd_ = kInvalidSocket;
    return Error("Failed to set socket to non-blocking mode");
  }

  ioMux_ = IoMultiplexer::create();
  if (!ioMux_ || !ioMux_->add(socketFd_, IoMultiplexer::Readable, true)) {
    ioMux_.reset();
    socketClose(socketFd_);
    socketFd_ = kInvalidSocket;
    return Error("Failed to add socket to I/O multiplexer");
  }

  listening_ = true;
  endpoint_.port = endpoint.port;
  return {};
}

TcpServer::Roe<int> TcpServer::accept() {
  if (!listening_) {
    return Error("Server not listening");
  }

  sockaddr_in client_addr {};
  socklen_t client_len = sizeof(client_addr);

  const SocketHandle client_fd =
      static_cast<SocketHandle>(::accept(socketFd_, reinterpret_cast<sockaddr*>(&client_addr),
                                         &client_len));
  if (client_fd < 0) {
    const int err = socketLastError();
    if (socketWouldBlock(err)) {
      return Error("No pending connections");
    }
    return Error("Failed to accept connection");
  }

  // Listen socket is non-blocking; some platforms inherit that on accept.
  // TcpServer callers (tests, blocking helpers) expect a blocking client fd.
  // FetchServer re-enables non-blocking after accept.
  if (!socketSetBlocking(client_fd)) {
    socketClose(client_fd);
    return Error("Failed to configure accepted socket");
  }

  return client_fd;
}

TcpServer::Roe<void> TcpServer::waitForEvents(int timeoutMs) {
  if (!listening_ || !ioMux_) {
    return Error("Server not listening");
  }

  std::vector<IoMultiplexer::ReadyEvent> ready;
  const int num_events = ioMux_->wait(timeoutMs, ready);
  if (num_events < 0) {
    return Error("I/O multiplexer wait failed");
  }
  if (num_events == 0) {
    return Error("Timeout waiting for events");
  }

  return {};
}

void TcpServer::stop() {
  ioMux_.reset();
  if (socketFd_ >= 0) {
    socketClose(socketFd_);
    socketFd_ = kInvalidSocket;
  }
  listening_ = false;
}

bool TcpServer::isListening() const { return listening_; }

IpEndpoint TcpServer::getEndpoint() const {
  IpEndpoint ep;
  ep.address = getHost();
  ep.port = endpoint_.port;
  return ep;
}

std::string TcpServer::getHost() const {
  if (!listening_ || socketFd_ < 0) {
    return endpoint_.address.empty() ? "localhost" : endpoint_.address;
  }
  return resolveAdvertisedHost(endpoint_.address, socketFd_);
}

} // namespace network
} // namespace pp
