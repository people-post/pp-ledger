#include "FetchServer.h"

namespace pp {
namespace network {

FetchServer::FetchServer()
    : Module("network.fetch_server")
    , running_(false)
    , port_(0) {
    log().info << "FetchServer initialized";
}

FetchServer::~FetchServer() {
    if (running_) {
        stop();
    }
}

bool FetchServer::start(uint16_t port, RequestHandler handler) {
    if (running_) {
        log().warning << "Server is already running";
        return false;
    }

    port_ = port;
    handler_ = std::move(handler);

    log().info << "Starting server on port: " + std::to_string(port_);

    // Start listening
    auto listenResult = server_.listen(port_);
    if (!listenResult) {
        std::string errorMsg = listenResult.error().message;
        log().error << "Failed to start listening: " + errorMsg;
        return false;
    }

    running_ = true;

    // Start server thread
    serverThread_ = std::thread(&FetchServer::serverLoop, this);

    log().info << "Server started successfully on port " + std::to_string(port_);
    return true;
}

void FetchServer::stop() {
    if (!running_) {
        log().warning << "Server is not running";
        return;
    }

    log().info << "Stopping server";
    
    running_ = false;
    server_.stop();
    
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    
    log().info << "Server stopped";
}

void FetchServer::serverLoop() {
    log().debug << "Server loop started";

    while (running_) {
        // Wait for events with a short timeout to allow checking running_ flag
        auto waitResult = server_.waitForEvents(100);  // 100ms timeout
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
        log().info << "Accepted connection from " + connection.getPeerAddress();

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

        // Process the request
        std::string response;
        try {
            response = handler_(request);
            log().debug << "Request processed successfully";
        } catch (const std::exception& e) {
            log().error << "Error processing request: " + std::string(e.what());
            response = "Error: " + std::string(e.what());
        }

        // Send response
        auto sendResult = connection.send(response);
        if (!sendResult) {
            std::string errorMsg = sendResult.error().message;
            log().error << "Failed to send response: " + errorMsg;
        } else {
            log().info << "Response sent (" + std::to_string(response.size()) + " bytes)";
        }

        connection.close();
        log().debug << "Connection closed";
    }

    log().debug << "Server loop ended";
}

} // namespace network
} // namespace pp
