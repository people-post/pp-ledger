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

  // Bind to a port and start listening
  Roe<void> listen(uint16_t port, int backlog = 10);

  // Accept a client connection (non-blocking)
  Roe<TcpConnection> accept();

  // Wait for events (timeout in milliseconds, -1 for infinite)
  Roe<void> waitForEvents(int timeoutMs = -1);

  // Stop the server
  void stop();

  // Check if server is listening
  bool isListening() const;

private:
  int socketFd_;
  int epollFd_;
  bool listening_;
  uint16_t port_;
};

} // namespace network
} // namespace pp
