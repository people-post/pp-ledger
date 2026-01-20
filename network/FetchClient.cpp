#include "FetchClient.h"
#include <thread>

namespace pp {
namespace network {

FetchClient::FetchClient() {
  setLogger("FetchClient");
  log().info << "FetchClient initialized";
}

void FetchClient::fetch(const TcpEndpoint &endpoint, const std::string &data,
                        ResponseCallback callback) {

  log().info << "Fetching from " << endpoint;

  // Run in a separate thread for async behavior
  std::thread([this, endpoint, data, callback]() {
    auto result = fetchSync(endpoint, data);
    callback(result);
  }).detach();
}

FetchClient::Roe<std::string>
FetchClient::fetchSync(const TcpEndpoint &endpoint, const std::string &data) {

  log().info << "Sync fetch from " << endpoint;

  TcpClient client;

  // Connect to the server
  auto connectResult = client.connect(endpoint);
  if (!connectResult) {
    std::string errorMsg = connectResult.error().message;
    log().error << "Failed to connect: " + errorMsg;
    return FetchClient::Error(1, "Failed to connect: " + errorMsg);
  }

  log().debug << "Connected successfully";

  // Send the data
  auto sendResult = client.send(data);
  if (!sendResult) {
    std::string errorMsg = sendResult.error().message;
    log().error << "Failed to send data: " + errorMsg;
    client.close();
    return FetchClient::Error(2, "Failed to send data: " + errorMsg);
  }

  log().debug << "Data sent, waiting for response";

  // Receive response
  char buffer[4096];
  auto recvResult = client.receive(buffer, sizeof(buffer) - 1);
  if (!recvResult) {
    std::string errorMsg = recvResult.error().message;
    log().error << "Failed to receive response: " + errorMsg;
    client.close();
    return FetchClient::Error(3, "Failed to receive response: " + errorMsg);
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
