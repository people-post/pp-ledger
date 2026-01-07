#include "FetchClient.h"
#include <libp2p/protocol/common/asio/asio_scheduler.hpp>
#include <libp2p/connection/stream.hpp>
#include <boost/asio/buffer.hpp>
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
        protocol,
        [this, data, callback](auto&& stream_res) {
            if (!stream_res) {
                log().error << "Failed to create stream: " + stream_res.error().message();
                callback(RoeErrorBase(1, "Failed to create stream: " + stream_res.error().message()));
                return;
            }

            auto stream = std::move(stream_res.value());
            log().debug << "Stream created successfully";

            // Send data to the peer
            auto send_buffer = std::make_shared<std::vector<uint8_t>>(data.begin(), data.end());
            stream->write(
                gsl::span<const uint8_t>(send_buffer->data(), send_buffer->size()),
                send_buffer->size(),
                [this, stream, callback, send_buffer](auto&& write_res) {
                    if (!write_res) {
                        log().error << "Failed to write data: " + write_res.error().message();
                        stream->close([](auto&&) {});
                        callback(RoeErrorBase(2, "Failed to write data: " + write_res.error().message()));
                        return;
                    }

                    log().debug << "Data sent, waiting for response";

                    // Read response from the peer
                    auto recv_buffer = std::make_shared<std::vector<uint8_t>>(4096);
                    stream->read(
                        gsl::span<uint8_t>(recv_buffer->data(), recv_buffer->size()),
                        recv_buffer->size(),
                        [this, stream, callback, recv_buffer](auto&& read_res) {
                            if (!read_res) {
                                log().error << "Failed to read response: " + read_res.error().message();
                                stream->close([](auto&&) {});
                                callback(RoeErrorBase(3, "Failed to read response: " + read_res.error().message()));
                                return;
                            }

                            size_t bytes_read = read_res.value();
                            std::string response(recv_buffer->begin(), recv_buffer->begin() + bytes_read);
                            
                            log().info << "Received response (" + std::to_string(bytes_read) + " bytes)";

                            // Close the stream
                            stream->close([callback, response](auto&& close_res) {
                                if (!close_res) {
                                    callback(RoeErrorBase(4, "Failed to close stream: " + close_res.error().message()));
                                } else {
                                    callback(response);
                                }
                            });
                        });
                });
        });
}

ResultOrError<std::string, RoeErrorBase> FetchClient::fetchSync(
    const libp2p::peer::PeerInfo& peerInfo,
    const std::string& protocol,
    const std::string& data) {
    
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    ResultOrError<std::string, RoeErrorBase> result = RoeErrorBase(5, "Timeout");

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
        return RoeErrorBase(5, "Fetch operation timed out");
    }

    return result;
}

} // namespace network
} // namespace pp
