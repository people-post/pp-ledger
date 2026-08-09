#pragma once

#include "lib/common/ResultOrError.hpp"
#include "TcpConnection.h"
#include "Types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pp {
namespace network {

class TcpClient {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  TcpClient();
  ~TcpClient();

  // Delete copy constructor and assignment
  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(const TcpClient &) = delete;

  // Allow move
  TcpClient(TcpClient &&other) noexcept;
  TcpClient &operator=(TcpClient &&other) noexcept;

  // Connect to a server
  Roe<void> connect(const IpEndpoint &endpoint);

  // Send data
  Roe<size_t> send(const void *data, size_t length);
  Roe<size_t> send(const std::string &message);

  // Send data and shutdown writing in one call
  Roe<size_t> sendAndShutdown(const void *data, size_t length);
  Roe<size_t> sendAndShutdown(const std::string &message);

  // Shutdown writing (half-close the connection)
  Roe<void> shutdownWrite();

  // Set socket send/receive timeout (0 = no timeout)
  Roe<void> setTimeout(std::chrono::milliseconds timeout);

  // Receive data
  Roe<size_t> receive(void *buffer, size_t maxLength);
  Roe<std::string> receiveLine();

  // Framed I/O (length-prefixed messages)
  Roe<void> writeFrame(std::string_view body);
  Roe<std::string> readFrame(std::chrono::milliseconds timeout);

  // Close connection
  void close();

  // Check if connected
  bool isConnected() const;

private:
  std::optional<TcpConnection> connection_;
};

} // namespace network
} // namespace pp
