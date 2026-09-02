#include "Server.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"

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

void Server::startRequestHandlers() {
  if (handlerPool_) {
    return;
  }

  size_t workers = performanceConfig_.handlerWorkers;
  if (workers == 0) {
    workers = defaultHandlerWorkers();
  }
  workers = std::clamp(workers, WorkerPool::kMinThreadCount, WorkerPool::kMaxThreadCount);

  handlerPool_ = std::make_unique<WorkerPool>(workers);
  handlerStop_.store(false);

  log().info << "Started " << workers << " RPC handler worker(s)";
}

void Server::stopRequestHandlers() {
  handlerStop_.store(true);
  if (handlerPool_) {
    handlerPool_->Shutdown();
    handlerPool_.reset();
  }
}

void Server::onStop() {
  stopAmpServer();
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

std::string Server::listenMultiaddr() const {
  return ampSupport_ ? ampSupport_->listenMultiaddr() : std::string{};
}

network::LedgerAmpRuntime* Server::ampRuntime() {
  return ampSupport_ ? &ampSupport_->runtime() : nullptr;
}

pp::amp::PeerLinkManager* Server::peerLinks() {
  return ampSupport_ ? &ampSupport_->links() : nullptr;
}

Service::Roe<void> Server::startAmpServer(const network::LedgerAmpConfig& config) {
  if (ampSupport_) {
    return Service::Error(-1, "AMP server already started");
  }
  ampSupport_ = std::make_unique<network::ServerAmpSupport>();
  startRequestHandlers();
  auto started = ampSupport_->Start(
      config, [this](const std::string& body) { return dispatchUnframedRequest(body); },
      handlerPool_.get());
  if (!started) {
    ampSupport_.reset();
    return started;
  }
  log().info << "AMP ledger listener: " << ampSupport_->listenMultiaddr();
  return {};
}

void Server::stopAmpServer() {
  stopRequestHandlers();
  if (ampSupport_) {
    ampSupport_->Stop();
    ampSupport_.reset();
  }
}

} // namespace pp
