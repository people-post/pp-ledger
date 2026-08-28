#pragma once

#include "common/ResultOrError.hpp"
#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pp {
namespace network {

class IoMultiplexer;

class TcpServer {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  TcpServer();
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  Roe<void> listen(const IpEndpoint& endpoint, int backlog = 10);
  Roe<int> accept();
  Roe<void> waitForEvents(int timeoutMs = -1);
  void stop();
  bool isListening() const;
  IpEndpoint getEndpoint() const;

private:
  std::string getHost() const;

  int socketFd_{-1};
  std::unique_ptr<IoMultiplexer> ioMux_;
  bool listening_{false};
  IpEndpoint endpoint_;
};

} // namespace network
} // namespace pp
