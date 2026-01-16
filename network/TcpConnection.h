#pragma once

#include "ResultOrError.hpp"

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

  // Receive data
  Roe<size_t> receive(void *buffer, size_t maxLength);
  Roe<std::string> receiveLine();

  // Close connection
  void close();

  // Get peer address
  std::string getPeerAddress() const;
  uint16_t getPeerPort() const;

private:
  int socketFd_;
  std::string peerAddress_;
  uint16_t peerPort_;
};

} // namespace network
} // namespace pp
