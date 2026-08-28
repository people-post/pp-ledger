#include "FetchClient.h"

#include "common/WorkerPool.h"

#include <mutex>

namespace pp {
namespace network {
namespace {

std::mutex gFetchPoolMutex;
std::unique_ptr<WorkerPool> gFetchPool;

WorkerPool& fetchPool() {
  std::lock_guard<std::mutex> lock(gFetchPoolMutex);
  if (!gFetchPool) {
    gFetchPool = std::make_unique<WorkerPool>(WorkerPool::kDefaultThreadCount);
  }
  return *gFetchPool;
}

} // namespace

FetchClient::FetchClient() = default;

void FetchClient::fetch(const IpEndpoint& endpoint, const std::string& data,
                        ResponseCallback callback,
                        std::chrono::milliseconds timeout) {
  log().info << "Fetching from " << endpoint;

  fetchPool().Post(WorkerLane::Normal,
                   [this, endpoint, data, callback = std::move(callback), timeout]() {
                     auto result = fetchSync(endpoint, data, timeout);
                     callback(result);
                   });
}

FetchClient::Roe<std::string>
FetchClient::fetchSync(const IpEndpoint& endpoint, const std::string& data,
                       std::chrono::milliseconds timeout) {
  log().debug << "Sync fetch from " << endpoint;

  TcpClient client;

  auto connectResult = client.connect(endpoint);
  if (!connectResult) {
    return Error(1, "Failed to connect: " + connectResult.error().message);
  }

  log().debug << "Connected successfully";

  if (timeout.count() > 0) {
    auto timeoutResult = client.setTimeout(timeout);
    if (!timeoutResult) {
      client.close();
      return Error(1, "Failed to set timeout: " + timeoutResult.error().message);
    }
  }

  auto writeResult = client.writeFrame(data);
  if (!writeResult) {
    client.close();
    return Error(2, "Failed to send data: " + writeResult.error().message);
  }

  log().debug << "Frame sent, waiting for response";

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
