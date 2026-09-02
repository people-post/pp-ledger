#include "Server.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include "platform/NetworkPlatform.h"

#include <algorithm>
#include <filesystem>
#include <thread>

namespace pp {
namespace {

bool isBootstrapWorkDirEntry(const std::filesystem::directory_entry& entry) {
  const std::string name = entry.path().filename().string();
  if (name == "config.json" || name == "init-config.json") {
    return entry.is_regular_file();
  }
  if (name == "keys") {
    return entry.is_directory();
  }
  if (entry.is_regular_file()) {
    return name.ends_with(".txt") || name.ends_with(".key");
  }
  return false;
}

size_t defaultHandlerWorkers() {
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) {
    return WorkerPool::kDefaultThreadCount;
  }
  return std::clamp(static_cast<size_t>(hw / 2), WorkerPool::kMinThreadCount,
                    WorkerPool::kMaxThreadCount);
}

} // namespace

Server::~Server() {
  stopRequestHandlers();
}

Service::Roe<void> Server::ensureWorkDirectory(const std::string& workDir,
                                                 const std::string& signatureFileName,
                                                 int32_t errorCode) {
  const std::filesystem::path workDirPath(workDir);
  const std::filesystem::path signaturePath = workDirPath / signatureFileName;

  if (!std::filesystem::exists(workDirPath)) {
    std::filesystem::create_directories(workDirPath);
    auto result = utl::writeToNewFile(signaturePath.string(), "");
    if (!result) {
      return Error(errorCode,
                   "Failed to create signature file: " + result.error().message);
    }
    return {};
  }

  if (std::filesystem::exists(signaturePath)) {
    return {};
  }

  for (const auto& entry : std::filesystem::directory_iterator(workDirPath)) {
    if (!isBootstrapWorkDirEntry(entry)) {
      return Error(errorCode,
                   "Work directory not recognized, please remove it "
                   "manually and try again");
    }
  }

  auto result = utl::writeToNewFile(signaturePath.string(), "");
  if (!result) {
    return Error(errorCode,
                 "Failed to create signature file: " + result.error().message);
  }
  return {};
}

Service::Roe<void> Server::run(const std::string& workDir) {
  workDir_ = workDir;

  if (useSignatureFile()) {
    auto ensured =
        ensureWorkDirectory(workDir, getSignatureFileName(), getRunErrorCode());
    if (!ensured) {
      return ensured;
    }
  }

  log().info << "Running " << getServerName() << " with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + getLogFileName(), logging::getLevel());

  return Service::run();
}

std::string Server::packResponse(const std::string& payload) {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = 0;
  resp.payload = payload;
  return utl::binaryPack(resp);
}

std::string Server::packResponse(uint16_t errorCode, const std::string& message) {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = errorCode;
  resp.payload = message;
  return utl::binaryPack(resp);
}

size_t Server::getRequestQueueSize() const { return requestQueue_.size(); }

bool Server::pollAndProcessOneRequest() {
  QueuedRequest qr;
  if (!requestQueue_.poll(qr)) {
    return false;
  }
  processQueuedRequest(qr);
  return true;
}

size_t Server::pollAndProcessAllRequests(size_t maxCount) {
  size_t n = 0;
  QueuedRequest qr;
  while (n < maxCount && requestQueue_.poll(qr)) {
    processQueuedRequest(qr);
    ++n;
  }
  return n;
}

void Server::processQueuedRequest(QueuedRequest& qr) {
  log().debug << "Processing request from queue";
  std::string response = handleRequest(qr.request);
  sendResponse(qr.fd, response);
}

void Server::handlerWorkerLoop() {
  while (!handlerStop_.load()) {
    QueuedRequest qr;
    if (!requestQueue_.waitPop(qr, std::chrono::milliseconds(50))) {
      continue;
    }
    processQueuedRequest(qr);
  }
}

void Server::startRequestHandlers() {
  if (handlerPool_) {
    return;
  }

  size_t workers = performanceConfig_.handlerWorkers;
  if (workers == 0) {
    workers = defaultHandlerWorkers();
  }
  workers = std::clamp(workers, WorkerPool::kMinThreadCount, WorkerPool::kMaxThreadCount);
  handlerWorkerCount_ = workers;

  handlerPool_ = std::make_unique<WorkerPool>(workers);
  handlerStop_.store(false);

  for (size_t i = 0; i < workers; ++i) {
    handlerPool_->Post(WorkerLane::Normal, [this]() { handlerWorkerLoop(); });
  }

  log().info << "Started " << workers << " RPC handler worker(s)";
}

void Server::stopRequestHandlers() {
  handlerStop_.store(true);
  if (handlerPool_) {
    handlerPool_->Shutdown();
    handlerPool_.reset();
  }
}

Service::Roe<void> Server::startFetchServer(const network::IpEndpoint& endpoint) {
  fetchServer_.redirectLogger(log().getFullName() + ".FetchServer");

  if (performanceConfig_.maxRequestQueueSize > 0) {
    requestQueue_.setMaxSize(performanceConfig_.maxRequestQueueSize);
  }

  network::FetchServer::Config config;
  config.endpoint = endpoint;
  config.performance = performanceConfig_;
  config.handler = [this](int fd, const std::string& request,
                          const network::IpEndpoint& peer) {
    QueuedRequest qr;
    qr.fd = fd;
    qr.request = request;
    if (!requestQueue_.tryPush(std::move(qr))) {
      log().warning << "Request queue full; rejecting request from " << peer.address;
      network::socketClose(fd);
      return;
    }
    log().debug << "Request enqueued (queue size: " << getRequestQueueSize() << ")";
  };

  customizeFetchServerConfig(config);

  auto started = fetchServer_.start(config);
  if (!started) {
    return started;
  }

  startRequestHandlers();
  return {};
}

void Server::stopFetchServer() {
  stopRequestHandlers();
  fetchServer_.stop();
}

void Server::onStop() {
  stopFetchServer();
#ifdef PP_LEDGER_HAS_AMP
  stopAmpServer();
#endif
}

void Server::sendResponse(int fd, const std::string& response) {
  auto addResponseResult = fetchServer_.tryWriteResponse(fd, response);
  if (!addResponseResult) {
    log().error << "Failed to queue response: " << addResponseResult.error().message;
    network::socketClose(fd);
  }
}

std::string Server::dispatchUnframedRequest(const std::string& requestBody) {
  return handleRequest(requestBody);
}

std::string Server::handleRequest(const std::string& request) {
  log().debug << "Received request (" << request.size() << " bytes)";
  auto reqResult = utl::binaryUnpack<Client::Request>(request);
  if (!reqResult) {
    return packResponse(1, reqResult.error().message);
  }
  return handleParsedRequest(reqResult.value());
}

#ifdef PP_LEDGER_HAS_AMP
Service::Roe<void> Server::startAmpServer(const network::LedgerAmpConfig& config) {
  if (ampSupport_) {
    return Service::Error(-1, "AMP server already started");
  }
  ampSupport_ = std::make_unique<network::ServerAmpSupport>();
  WorkerPool* pool = handlerPool_ ? handlerPool_.get() : nullptr;
  if (!pool && !handlerPool_) {
    startRequestHandlers();
    pool = handlerPool_.get();
  }
  auto started = ampSupport_->Start(
      config, [this](const std::string& body) { return dispatchUnframedRequest(body); }, pool);
  if (!started) {
    ampSupport_.reset();
    return started;
  }
  log().info << "AMP ledger listener: " << ampSupport_->listenMultiaddr();
  return {};
}

void Server::stopAmpServer() {
  if (ampSupport_) {
    ampSupport_->Stop();
    ampSupport_.reset();
  }
}
#endif

} // namespace pp
