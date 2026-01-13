#include "FetchClient.h"
#include <libp2p/connection/stream.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace pp {
namespace network {

FetchClient::FetchClient(std::shared_ptr<libp2p::Host> host)
    : Module("network.fetch_client")
    , host_(std::move(host)) {
    log().info << "FetchClient initialized";
}

void FetchClient::fetch(
    const libp2p::peer::PeerInfo& peerInfo,
    const std::string& protocol,
    const std::string& data,
    ResponseCallback callback) {
    
    log().info << "Fetching from peer with protocol: " + protocol;

    // Create a new stream to the peer
    host_->newStream(
        peerInfo,
        {protocol},  // StreamProtocols is a vector
        [this, data, callback](libp2p::StreamAndProtocolOrError stream_res) {
            if (!stream_res) {
                log().error << "Failed to create stream: " + stream_res.error().message();
                callback(FetchClient::Error(1, "Failed to create stream: " + stream_res.error().message()));
                return;
            }

            auto stream = std::move(stream_res.value().stream);
            log().debug << "Stream created successfully";

            // Send data to the peer
            auto send_buffer = std::make_shared<std::vector<uint8_t>>(data.begin(), data.end());
            stream->writeSome(
                libp2p::BytesIn(*send_buffer),
                [this, stream, callback, send_buffer](outcome::result<size_t> write_res) {
                    if (!write_res) {
                        log().error << "Failed to write data: " + write_res.error().message();
                        stream->close([](auto&&) {});
                        callback(FetchClient::Error(2, "Failed to write data: " + write_res.error().message()));
                        return;
                    }

                    log().debug << "Data sent, waiting for response";

                    // Read response from the peer
                    auto recv_buffer = std::make_shared<std::vector<uint8_t>>(4096);
                    stream->readSome(
                        libp2p::BytesOut(*recv_buffer),
                        [this, stream, callback, recv_buffer](outcome::result<size_t> read_res) {
                            if (!read_res) {
                                log().error << "Failed to read response: " + read_res.error().message();
                                stream->close([](auto&&) {});
                                callback(FetchClient::Error(3, "Failed to read response: " + read_res.error().message()));
                                return;
                            }

                            size_t bytes_read = read_res.value();
                            std::string response(recv_buffer->begin(), recv_buffer->begin() + bytes_read);
                            
                            log().info << "Received response (" + std::to_string(bytes_read) + " bytes)";

                            // Close the stream and send response
                            stream->close([callback, response](outcome::result<void> close_res) {
                                if (!close_res) {
                                    callback(FetchClient::Error(4, "Failed to close stream: " + close_res.error().message()));
                                } else {
                                    callback(response);
                                }
                            });
                        });
                });
        });
}

FetchClient::Roe<std::string> FetchClient::fetchSync(
    const libp2p::peer::PeerInfo& peerInfo,
    const std::string& protocol,
    const std::string& data) {
    
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    FetchClient::Roe<std::string> result = FetchClient::Error(5, "Timeout");

    fetch(peerInfo, protocol, data, [&](const auto& res) {
        std::lock_guard<std::mutex> lock(mtx);
        result = res;
        done = true;
        cv.notify_one();
    });

    // Wait for response with timeout
    std::unique_lock<std::mutex> lock(mtx);
    if (!cv.wait_for(lock, std::chrono::seconds(30), [&done] { return done; })) {
        log().error << "Fetch operation timed out";
        return FetchClient::Error(5, "Fetch operation timed out");
    }

    return result;
}

} // namespace network
} // namespace pp
