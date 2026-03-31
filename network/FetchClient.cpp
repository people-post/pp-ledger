#include "FetchClient.h"
#include <thread>

namespace pp {
namespace network {

FetchClient::FetchClient() {}

void FetchClient::fetch(const IpEndpoint &endpoint, const std::string &data,
                        ResponseCallback callback,
                        std::chrono::milliseconds timeout) {

  log().info << "Fetching from " << endpoint;

  // Run in a separate thread for async behavior
  std::thread([this, endpoint, data, callback = std::move(callback), timeout]() {
    auto result = fetchSync(endpoint, data, timeout);
    callback(result);
  }).detach();
}

FetchClient::Roe<std::string>
FetchClient::fetchSync(const IpEndpoint &endpoint, const std::string &data,
                       std::chrono::milliseconds timeout) {
  log().debug << "Sync fetch from " << endpoint;

  TcpClient client;

  // Connect to the server
  auto connectResult = client.connect(endpoint);
  if (!connectResult) {
    return Error(1, "Failed to connect: " + connectResult.error().message);
  }

  log().debug << "Connected successfully";

  // Apply socket timeout for send and receive operations
  if (timeout.count() > 0) {
    auto timeoutResult = client.setTimeout(timeout);
    if (!timeoutResult) {
      client.close();
      return Error(1, "Failed to set timeout: " + timeoutResult.error().message);
    }
  }

  // Send framed request (length + body)
  auto writeResult = client.writeFrame(data);
  if (!writeResult) {
    client.close();
    return Error(2, "Failed to send data: " + writeResult.error().message);
  }

  log().debug << "Frame sent, waiting for response";

  // Receive framed response (length + body)
  auto readResult = client.readFrame(timeout);
  if (!readResult) {
    client.close();
    return Error(3, "Failed to receive response: " + readResult.error().message);
  }

  std::string response = std::move(readResult.value());

  log().debug << "Received response (" + std::to_string(response.size()) + " bytes)";

  client.close();
  return response;
}

} // namespace network
} // namespace pp
