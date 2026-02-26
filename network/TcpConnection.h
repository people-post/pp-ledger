#pragma once

#include "ResultOrError.hpp"
#include "Types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pp {
namespace network {

class TcpConnection {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  TcpConnection(int socket_fd);
  ~TcpConnection();

  // Delete copy
  TcpConnection(const TcpConnection &) = delete;
  TcpConnection &operator=(const TcpConnection &) = delete;

  // Allow move
  TcpConnection(TcpConnection &&other) noexcept;
  TcpConnection &operator=(TcpConnection &&other) noexcept;

  // Send data
  Roe<size_t> send(const void *data, size_t length);
  Roe<size_t> send(const std::string &message);

  // Send data and shutdown writing in one call
  Roe<size_t> sendAndShutdown(const void *data, size_t length);
  Roe<size_t> sendAndShutdown(const std::string &message);

  // Shutdown writing (half-close the connection)
  Roe<void> shutdownWrite();

  // Receive data
  Roe<size_t> receive(void *buffer, size_t maxLength);
  Roe<std::string> receiveLine();

  // Set socket send/receive timeout (0 = no timeout)
  Roe<void> setTimeout(std::chrono::milliseconds timeout);

  // Close connection
  void close();

  // Get peer endpoint
  const IpEndpoint &getPeerEndpoint() const;

private:
  int socketFd_;
  IpEndpoint peer_;
};

} // namespace network
} // namespace pp
