#pragma once

#include "BulkWriter.h"
#include "ResultOrError.hpp"
#include "Service.h"
#include "TcpServer.h"
#include "TcpConnection.h"
#include "Types.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace pp {
namespace network {

/**
 * FetchServer - Simple server for receiving data and sending responses
 *
 * Uses TCP sockets for peer-to-peer communication.
 * Handles multiple concurrent connections using non-blocking I/O.
 */
class FetchServer : public Service {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  using RequestHandler = std::function<void(int fd, const std::string&, const TcpEndpoint& endpoint)>;

  struct Config {
    TcpEndpoint endpoint;
    RequestHandler handler{ nullptr };
    std::vector<std::string> whitelist;
  };

  /**
   * Constructor
   */
  FetchServer();

  ~FetchServer() override;

  TcpEndpoint getEndpoint() const { return server_.getEndpoint(); }
  Roe<void> addResponse(int fd, const std::string &response);
  Service::Roe<void> start(const Config &config);
protected:
  void runLoop() override;

  Service::Roe<void> onStart() override;
  void onStop() override;

private:
  // Tracks an active connection reading data
  struct ActiveConnection {
    int fd;
    std::string buffer;
    TcpEndpoint endpoint;
  };

  // Helper: set a file descriptor to non-blocking mode
  bool setNonBlocking(int fd);

  // Helper: get peer endpoint for a connected socket
  Roe<TcpEndpoint> getPeerEndpoint(int fd);

  // Helper: true if peer is allowed by whitelist (empty whitelist = allow all)
  bool isAllowedByWhitelist(const TcpEndpoint& peer) const;

  // Helper: process read events from epoll
  void processReadEvents(const std::vector<int>& readyFds);

  // Helper: read available data from a connection
  void readFromConnection(ActiveConnection& conn);

  TcpServer server_;
  Config config_;
  BulkWriter writer_;

  // epoll file descriptor for monitoring connections
#ifdef __APPLE__
  int kqueueFd_{ -1 };
#else
  int epollFd_{ -1 };
#endif

  // Map of fd -> ActiveConnection for all connections being read
  std::map<int, ActiveConnection> activeConnections_;
};

} // namespace network
} // namespace pp
