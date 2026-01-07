#include "FetchServer.h"

namespace pp {
namespace network {

FetchServer::FetchServer(std::shared_ptr<libp2p::Host> host)
    : Module("network.fetch_server")
    , host_(std::move(host))
    , running_(false) {
    log().info << "FetchServer initialized";
}

FetchServer::~FetchServer() {
    if (running_) {
        stop();
    }
}

void FetchServer::start(const std::string& protocol, RequestHandler handler) {
    if (running_) {
        log().warning << "Server is already running";
        return;
    }

    protocol_ = protocol;
    handler_ = std::move(handler);
    running_ = true;

    log().info << "Starting server on protocol: " + protocol_;

    // Set protocol handler
    host_->setProtocolHandler(
        {protocol_},  // StreamProtocols is a vector
        [this](libp2p::StreamAndProtocol stream_and_protocol) {
            auto stream = stream_and_protocol.stream;
            log().info << "Accepted new connection";
            handleStream(stream);
        });

    log().info << "Server started successfully";
}

void FetchServer::stop() {
    if (!running_) {
        log().warning << "Server is not running";
        return;
    }

    log().info << "Stopping server";
    
    // Remove protocol handler
    if (!protocol_.empty()) {
        host_->setProtocolHandler({protocol_}, nullptr);
    }
    
    running_ = false;
    log().info << "Server stopped";
}

void FetchServer::handleStream(std::shared_ptr<libp2p::connection::Stream> stream) {
    log().debug << "Handling incoming stream";

    // Read data from the peer
    auto recv_buffer = std::make_shared<std::vector<uint8_t>>(4096);
    stream->readSome(
        libp2p::BytesOut(*recv_buffer),
        [this, stream, recv_buffer](outcome::result<size_t> read_res) {
            if (!read_res) {
                log().error << "Failed to read data: " + read_res.error().message();
                stream->close([](auto&&) {});
                return;
            }

            size_t bytes_read = read_res.value();
            std::string request(recv_buffer->begin(), recv_buffer->begin() + bytes_read);
            
            log().info << "Received request (" + std::to_string(bytes_read) + " bytes)";

            // Process the request
            std::string response;
            try {
                response = handler_(request);
                log().debug << "Request processed successfully";
            } catch (const std::exception& e) {
                log().error << "Error processing request: " + std::string(e.what());
                response = "Error: " + std::string(e.what());
            }

            // Send response back to the peer
            auto send_buffer = std::make_shared<std::vector<uint8_t>>(response.begin(), response.end());
            stream->writeSome(
                libp2p::BytesIn(*send_buffer),
                [this, stream, send_buffer](outcome::result<size_t> write_res) {
                    if (!write_res) {
                        log().error << "Failed to write response: " + write_res.error().message();
                    } else {
                        log().info << "Response sent (" + std::to_string(send_buffer->size()) + " bytes)";
                    }

                    // Close the stream
                    stream->close([this](outcome::result<void> close_res) {
                        if (!close_res) {
                            log().error << "Failed to close stream: " + close_res.error().message();
                        } else {
                            log().debug << "Stream closed";
                        }
                    });
                });
        });
}

} // namespace network
} // namespace pp
