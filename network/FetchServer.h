#pragma once

#include "ResultOrError.hpp"
#include "Service.h"
#include "TcpServer.h"
#include "TcpConnection.h"
#include "Types.hpp"
#include <functional>
#include <memory>
#include <string>

namespace pp {
namespace network {

/**
 * FetchServer - Simple server for receiving data and sending responses
 *
 * Uses TCP sockets for peer-to-peer communication.
 * Simple pattern: accept connection, receive, send response, close.
 */
class FetchServer : public Service {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  using RequestHandler = std::function<void(const std::string&, std::shared_ptr<TcpConnection>)>;

  struct Config {
    TcpEndpoint endpoint;
    RequestHandler handler{ nullptr };
  };

  /**
   * Constructor
   */
  FetchServer();

  ~FetchServer() override = default;

  /**
   * Start the server on specified host and port
   * @param config Configuration for the server
   * @return true if server started successfully
   */
  Service::Roe<void> start(const Config &config);

  /**
   * Get the port the server is listening on
   */
  uint16_t getPort() const { return server_.getPort(); }

  /**
   * Get the host address the server is bound to
   * Returns the external address when able to accept non-local connections
   */
  std::string getHost() const { return server_.getHost(); }

protected:
  /**
   * Service thread main loop - accepts and handles connections
   */
  void runLoop() override;

  /**
   * Called before service thread starts - sets up TCP listener
   */
  Service::Roe<void> onStart() override;

  /**
   * Called after service thread stops - cleans up TCP server
   */
  void onStop() override;

private:
  TcpServer server_;
  Config config_;
};

} // namespace network
} // namespace pp
