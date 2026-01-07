#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include <libp2p/host/host.hpp>
#include <libp2p/peer/peer_info.hpp>
#include <memory>
#include <string>
#include <functional>

namespace pp {
namespace network {

/**
 * FetchClient - Simple client for sending data and receiving responses
 * 
 * Uses libp2p for peer-to-peer communication without HTTP protocol.
 * Simple pattern: connect, send, receive, close.
 */
class FetchClient : public Module {
public:
    using ResponseCallback = std::function<void(const ResultOrError<std::string, RoeErrorBase>&)>;

    /**
     * Constructor
     * @param host Shared pointer to libp2p host
     */
    explicit FetchClient(std::shared_ptr<libp2p::Host> host);
    
    ~FetchClient() override = default;

    /**
     * Fetch data from a remote peer
     * @param peerInfo Information about the peer to connect to
     * @param protocol Protocol identifier for the connection
     * @param data Data to send to the peer
     * @param callback Callback function to receive the response
     */
    void fetch(
        const libp2p::peer::PeerInfo& peerInfo,
        const std::string& protocol,
        const std::string& data,
        ResponseCallback callback);

    /**
     * Synchronous fetch - blocks until response is received
     * @param peerInfo Information about the peer to connect to
     * @param protocol Protocol identifier for the connection
     * @param data Data to send to the peer
     * @return Response data or error
     */
    ResultOrError<std::string, RoeErrorBase> fetchSync(
        const libp2p::peer::PeerInfo& peerInfo,
        const std::string& protocol,
        const std::string& data);

private:
    std::shared_ptr<libp2p::Host> host_;
};

} // namespace network
} // namespace pp
