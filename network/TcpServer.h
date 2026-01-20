#pragma once

#include "ResultOrError.hpp"
#include "TcpConnection.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pp {
namespace network {

class TcpServer {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  TcpServer();
  ~TcpServer();

  // Delete copy
  TcpServer(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &) = delete;

  // Bind to a host and port and start listening
  Roe<void> listen(const std::string &host, uint16_t port, int backlog = 10);

  // Accept a client connection (non-blocking)
  Roe<TcpConnection> accept();

  // Wait for events (timeout in milliseconds, -1 for infinite)
  Roe<void> waitForEvents(int timeoutMs = -1);

  // Stop the server
  void stop();

  // Check if server is listening
  bool isListening() const;

  // Get the host address the server is bound to
  // Returns the external address when able to accept non-local connections
  std::string getHost() const;

  // Get the port the server is listening on
  uint16_t getPort() const { return port_; }

private:
  // Helper to get the actual bound address
  std::string getBoundAddress() const;

  int socketFd_;
#ifdef __APPLE__
  int kqueueFd_;
#else
  int epollFd_;
#endif
  bool listening_;
  uint16_t port_;
  std::string originalHost_; // Store original host string
};

} // namespace network
} // namespace pp
