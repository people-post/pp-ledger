#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include "TcpServer.h"
#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace pp {
namespace network {

/**
 * FetchServer - Simple server for receiving data and sending responses
 * 
 * Uses TCP sockets for peer-to-peer communication.
 * Simple pattern: accept connection, receive, send response, close.
 */
class FetchServer : public Module {
public:
    using RequestHandler = std::function<std::string(const std::string&)>;

    /**
     * Constructor
     */
    FetchServer();
    
    ~FetchServer() override;

    /**
     * Start the server on specified port
     * @param port Port to listen on
     * @param handler Function to handle incoming requests
     * @return true if server started successfully
     */
    bool start(uint16_t port, RequestHandler handler);

    /**
     * Stop the server
     */
    void stop();

    /**
     * Check if server is running
     */
    bool isRunning() const { return running_; }

    /**
     * Get the port the server is listening on
     */
    uint16_t getPort() const { return port_; }

private:
    /**
     * Server loop that accepts and handles connections
     */
    void serverLoop();

    TcpServer server_;
    RequestHandler handler_;
    std::atomic<bool> running_;
    uint16_t port_;
    std::thread serverThread_;
};

} // namespace network
} // namespace pp
