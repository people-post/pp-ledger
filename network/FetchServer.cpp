#include "FetchServer.h"
#include <string>

namespace pp {
namespace network {

FetchServer::FetchServer() {
  setLogger("FetchServer");
  log().info << "FetchServer initialized";
}

bool FetchServer::start(const Config &config) {
  if (isRunning()) {
    log().warning << "Server is already running";
    return false;
  }

  config_ = config;

  log().info << "Starting server on " << config_.endpoint.address << ":"
             << config_.endpoint.port;

  // Call base class start() which will call onStart() then spawn thread
  return Service::start();
}

bool FetchServer::onStart() {
  // Start listening
  auto listenResult = server_.listen(config_.endpoint);
  if (!listenResult) {
    std::string errorMsg = listenResult.error().message;
    log().error << "Failed to start listening: " + errorMsg;
    return false;
  }

  log().info << "Server started successfully on " << config_.endpoint.address
             << ":" << config_.endpoint.port;
  return true;
}

void FetchServer::onStop() { server_.stop(); }

void FetchServer::run() {
  log().debug << "Server loop started";

  while (isRunning()) {
    // Wait for events with a short timeout to allow checking running_ flag
    auto waitResult = server_.waitForEvents(100); // 100ms timeout
    if (!waitResult) {
      // Timeout or error - just continue
      continue;
    }

    // Accept a connection
    auto acceptResult = server_.accept();
    if (!acceptResult) {
      // No connection waiting or error
      continue;
    }

    auto connection = std::move(acceptResult.value());
    log().info << "Accepted connection from " << connection.getPeerEndpoint();

    // Read request data
    char buffer[4096];
    auto recvResult = connection.receive(buffer, sizeof(buffer) - 1);
    if (!recvResult) {
      std::string errorMsg = recvResult.error().message;
      log().error << "Failed to read data: " + errorMsg;
      connection.close();
      continue;
    }

    size_t bytesRead = recvResult.value();
    buffer[bytesRead] = '\0';
    std::string request(buffer, bytesRead);

    log().info << "Received request (" + std::to_string(bytesRead) + " bytes)";

    // Process the request - handler now owns the connection and response
    try {
      auto connPtr = std::make_shared<TcpConnection>(std::move(connection));
      config_.handler(request, connPtr);
      log().debug << "Request processed successfully";
    } catch (const std::exception &e) {
      log().error << "Error processing request: " + std::string(e.what());
    }
  }

  log().debug << "Server loop ended";
}

} // namespace network
} // namespace pp
