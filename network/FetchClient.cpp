#include "FetchClient.h"
#include <thread>

namespace pp {
namespace network {

FetchClient::FetchClient()
    : Module("network.fetch_client") {
    log().info << "FetchClient initialized";
}

void FetchClient::fetch(
    const std::string& host,
    uint16_t port,
    const std::string& data,
    ResponseCallback callback) {
    
    log().info << "Fetching from " + host + ":" + std::to_string(port);

    // Run in a separate thread for async behavior
    std::thread([this, host, port, data, callback]() {
        auto result = fetchSync(host, port, data);
        callback(result);
    }).detach();
}

FetchClient::Roe<std::string> FetchClient::fetchSync(
    const std::string& host,
    uint16_t port,
    const std::string& data) {
    
    log().info << "Sync fetch from " + host + ":" + std::to_string(port);

    TcpClient client;
    
    // Connect to the server
    auto connectResult = client.connect(host, port);
    if (!connectResult) {
        log().error << "Failed to connect: " + connectResult.error().message;
        return FetchClient::Error(1, "Failed to connect: " + connectResult.error().message);
    }
    
    log().debug << "Connected successfully";
    
    // Send the data
    auto sendResult = client.send(data);
    if (!sendResult) {
        log().error << "Failed to send data: " + sendResult.error().message;
        client.close();
        return FetchClient::Error(2, "Failed to send data: " + sendResult.error().message);
    }
    
    log().debug << "Data sent, waiting for response";
    
    // Receive response
    char buffer[4096];
    auto recvResult = client.receive(buffer, sizeof(buffer) - 1);
    if (!recvResult) {
        log().error << "Failed to receive response: " + recvResult.error().message;
        client.close();
        return FetchClient::Error(3, "Failed to receive response: " + recvResult.error().message);
    }
    
    size_t bytesRead = recvResult.value();
    buffer[bytesRead] = '\0';
    std::string response(buffer, bytesRead);
    
    log().info << "Received response (" + std::to_string(bytesRead) + " bytes)";
    
    client.close();
    return response;
}

} // namespace network
} // namespace pp
