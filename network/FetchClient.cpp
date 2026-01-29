#include "FetchClient.h"
#include <thread>

namespace pp {
namespace network {

FetchClient::FetchClient() {}

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
  log().debug << "Sync fetch from " << endpoint;

  TcpClient client;

  // Connect to the server
  auto connectResult = client.connect(endpoint);
  if (!connectResult) {
    return Error(1, "Failed to connect: " + connectResult.error().message);
  }

  log().debug << "Connected successfully";

  // Send the data and shutdown writing
  auto sendResult = client.send(data);
  if (!sendResult) {
    client.close();
    return Error(2, "Failed to send data: " + sendResult.error().message);
  }

  // Shutdown writing to signal end of data
  auto shutdownResult = client.shutdownWrite();
  if (!shutdownResult) {
    client.close();
    return Error(2, "Failed to shutdown write: " + shutdownResult.error().message);
  }

  log().debug << "Data sent and write shutdown, waiting for response";

  // Receive response
  char buffer[4096];
  auto recvResult = client.receive(buffer, sizeof(buffer) - 1);
  if (!recvResult) {
    client.close();
    return Error(3, "Failed to receive response: " + recvResult.error().message);
  }

  size_t bytesRead = recvResult.value();
  buffer[bytesRead] = '\0';
  std::string response(buffer, bytesRead);

  log().debug << "Received response (" + std::to_string(bytesRead) + " bytes)";

  client.close();
  return response;
}

} // namespace network
} // namespace pp
