#pragma once

#include "ResultOrError.hpp"
#include "Service.h"
#include "TcpServer.h"
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
  using RequestHandler = std::function<std::string(const std::string &)>;

  /**
   * Constructor
   */
  FetchServer();

  ~FetchServer() override = default;

  /**
   * Start the server on specified port
   * @param port Port to listen on
   * @param handler Function to handle incoming requests
   * @return true if server started successfully
   */
  bool start(uint16_t port, RequestHandler handler);

  /**
   * Get the port the server is listening on
   */
  uint16_t getPort() const { return port_; }

protected:
  /**
   * Service thread main loop - accepts and handles connections
   */
  void run() override;

  /**
   * Called before service thread starts - sets up TCP listener
   */
  bool onStart() override;

  /**
   * Called after service thread stops - cleans up TCP server
   */
  void onStop() override;

private:
  TcpServer server_;
  RequestHandler handler_;
  uint16_t port_;
};

} // namespace network
} // namespace pp
