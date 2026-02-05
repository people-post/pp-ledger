#include "FetchServer.h"
#include <string>

namespace pp {
namespace network {

FetchServer::FetchServer() {}

Service::Roe<void> FetchServer::start(const Config &config) {
  if (!isStopSet()) {
    return Service::Error(-1, "Server is already running");
  }

  config_ = config;

  log().info << "Starting server on " << config_.endpoint.address << ":"
             << config_.endpoint.port;

  // Call base class start() which will call onStart() then spawn thread
  return Service::start();
}

Service::Roe<void> FetchServer::onStart() {
  // Start listening
  auto listenResult = server_.listen(config_.endpoint);
  if (!listenResult) {
    return Service::Error(-2, "Failed to start listening: " + listenResult.error().message);
  }

  return {};
}

void FetchServer::onStop() { server_.stop(); }

void FetchServer::runLoop() {
  log().debug << "Server loop started";

  while (!isStopSet()) {
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

    // Read all request data until client shutdown write
    std::string request;
    char buffer[4096];
    bool readError = false;
    while (true) {
      auto recvResult = connection.receive(buffer, sizeof(buffer));
      if (!recvResult) {
        std::string errorMsg = recvResult.error().message;
        // Check if this is a clean shutdown (client closed write side)
        if (errorMsg.find("closed by peer") != std::string::npos) {
          // Expected shutdown signal from client
          log().debug << "Client shutdown writing, received all data";
          break;
        }
        log().error << "Failed to read data: " + errorMsg;
        connection.close();
        readError = true;
        break;
      }

      size_t bytesRead = recvResult.value();
      request.append(buffer, bytesRead);
    }

    if (readError) {
      continue;
    }

    log().info << "Received complete request (" + std::to_string(request.length()) + " bytes)";

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
