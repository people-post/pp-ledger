#pragma once

#include "ResultOrError.hpp"
#include "Types.hpp"

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
  Roe<void> listen(const IpEndpoint &endpoint, int backlog = 10);

  // Accept a client connection (non-blocking)
  Roe<int> accept();

  // Wait for events (timeout in milliseconds, -1 for infinite)
  Roe<void> waitForEvents(int timeoutMs = -1);

  // Stop the server
  void stop();

  // Check if server is listening
  bool isListening() const;

  IpEndpoint getEndpoint() const;

private:
  std::string getHost() const;
  // Helper to get the actual bound address
  std::string getBoundAddress() const;

  int socketFd_{ -1 };
#ifdef __APPLE__
  int kqueueFd_{ -1 };
#else
  int epollFd_{ -1 };
#endif
  bool listening_{ false };
  IpEndpoint endpoint_;
};

} // namespace network
} // namespace pp
