#pragma once

#include "ResultOrError.hpp"
#include "TcpConnection.h"
#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

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
  Roe<void> connect(const TcpEndpoint &endpoint);

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

  // Close connection
  void close();

  // Check if connected
  bool isConnected() const;

private:
  std::optional<TcpConnection> connection_;
};

} // namespace network
} // namespace pp
