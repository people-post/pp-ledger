#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include <libp2p/host/host.hpp>
#include <libp2p/connection/stream.hpp>
#include <memory>
#include <string>
#include <functional>

namespace pp {
namespace network {

/**
 * FetchServer - Simple server for receiving data and sending responses
 * 
 * Uses libp2p for peer-to-peer communication without HTTP protocol.
 * Simple pattern: accept connection, receive, send response, close.
 */
class FetchServer : public Module {
public:
    using RequestHandler = std::function<std::string(const std::string&)>;

    /**
     * Constructor
     * @param host Shared pointer to libp2p host
     */
    explicit FetchServer(std::shared_ptr<libp2p::Host> host);
    
    ~FetchServer() override;

    /**
     * Start the server and register a protocol handler
     * @param protocol Protocol identifier to handle
     * @param handler Function to handle incoming requests
     */
    void start(const std::string& protocol, RequestHandler handler);

    /**
     * Stop the server
     */
    void stop();

    /**
     * Check if server is running
     */
    bool isRunning() const { return running_; }

private:
    /**
     * Handle an incoming stream
     */
    void handleStream(std::shared_ptr<libp2p::connection::Stream> stream);

    std::shared_ptr<libp2p::Host> host_;
    RequestHandler handler_;
    bool running_;
    std::string protocol_;
};

} // namespace network
} // namespace pp
