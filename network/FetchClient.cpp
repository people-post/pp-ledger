#include "FetchClient.h"
#include <thread>

namespace pp {
namespace network {

FetchClient::FetchClient() {}

void FetchClient::fetch(const TcpEndpoint &endpoint, const std::string &data,
                        ResponseCallback callback,
                        std::chrono::milliseconds timeout) {

  log().info << "Fetching from " << endpoint;

  // Run in a separate thread for async behavior
  std::thread([this, endpoint, data, callback, timeout]() {
    auto result = fetchSync(endpoint, data, timeout);
    callback(result);
  }).detach();
}

FetchClient::Roe<std::string>
FetchClient::fetchSync(const TcpEndpoint &endpoint, const std::string &data,
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

  // Receive full response (server sends entire payload then closes connection)
  std::string response;
  const size_t chunkSize = 8192;
  char buffer[chunkSize];
  while (client.isConnected()) {
    auto recvResult = client.receive(buffer, sizeof(buffer));
    if (!recvResult) {
      if (!response.empty()) {
        break; // Partial data received then error; return what we have
      }
      client.close();
      return Error(3, "Failed to receive response: " + recvResult.error().message);
    }
    size_t bytesRead = recvResult.value();
    if (bytesRead == 0) {
      break; // Server closed connection
    }
    response.append(buffer, bytesRead);
  }

  log().debug << "Received response (" + std::to_string(response.size()) + " bytes)";

  client.close();
  return response;
}

} // namespace network
} // namespace pp
