#include "Server.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Logger.h"
#include "lib/common/Utilities.h"
#include <filesystem>

namespace pp {
namespace {

bool isBootstrapWorkDirEntry(const std::filesystem::directory_entry &entry) {
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

} // namespace

Service::Roe<void> Server::ensureWorkDirectory(
    const std::string &workDir, const std::string &signatureFileName,
    int32_t errorCode) {
  const std::filesystem::path workDirPath(workDir);
  const std::filesystem::path signaturePath = workDirPath / signatureFileName;

  if (!std::filesystem::exists(workDirPath)) {
    std::filesystem::create_directories(workDirPath);
    auto result = utl::writeToNewFile(signaturePath.string(), "");
    if (!result) {
      return Error(errorCode,
                   "Failed to create signature file: " +
                       result.error().message);
    }
    return {};
  }

  if (std::filesystem::exists(signaturePath)) {
    return {};
  }

  // Directory exists without a signature (common for empty Docker volume
  // mounts). Allow bootstrap-only contents; reject anything else.
  for (const auto &entry : std::filesystem::directory_iterator(workDirPath)) {
    if (!isBootstrapWorkDirEntry(entry)) {
      return Error(errorCode,
                   "Work directory not recognized, please remove it "
                   "manually and try again");
    }
  }

  auto result = utl::writeToNewFile(signaturePath.string(), "");
  if (!result) {
    return Error(errorCode,
                 "Failed to create signature file: " +
                     result.error().message);
  }
  return {};
}

Service::Roe<void> Server::run(const std::string &workDir) {
  workDir_ = workDir;

  if (useSignatureFile()) {
    auto ensured =
        ensureWorkDirectory(workDir, getSignatureFileName(), getRunErrorCode());
    if (!ensured) {
      return ensured;
    }
  }

  log().info << "Running " << getServerName()
             << " with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + getLogFileName(), logging::getLevel());

  return Service::run();
}

std::string Server::packResponse(const std::string &payload) {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = 0;
  resp.payload = payload;
  return utl::binaryPack(resp);
}

std::string Server::packResponse(uint16_t errorCode,
                                 const std::string &message) {
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

void Server::processQueuedRequest(QueuedRequest &qr) {
  log().debug << "Processing request from queue";
  std::string response = handleRequest(qr.request);
  sendResponse(qr.fd, response);
}

Service::Roe<void>
Server::startFetchServer(const network::IpEndpoint &endpoint) {
  fetchServer_.redirectLogger(log().getFullName() + ".FetchServer");
  network::FetchServer::Config config;
  config.endpoint = endpoint;
  config.handler = [this](int fd, const std::string &request,
                          const network::IpEndpoint &) {
    requestQueue_.push(QueuedRequest{fd, request});

    log().debug << "Request enqueued (queue size: " << getRequestQueueSize()
                << ")";
  };
  customizeFetchServerConfig(config);
  return fetchServer_.start(config);
}

void Server::stopFetchServer() { fetchServer_.stop(); }

void Server::onStop() { stopFetchServer(); }

void Server::sendResponse(int fd, const std::string &response) {
  auto addResponseResult = fetchServer_.addResponse(fd, response);
  if (!addResponseResult) {
    log().error << "Failed to queue response: "
                << addResponseResult.error().message;
  }
}

std::string Server::handleRequest(const std::string &request) {
  log().debug << "Received request (" << request.size() << " bytes)";
  auto reqResult = utl::binaryUnpack<Client::Request>(request);
  if (!reqResult) {
    return packResponse(1, reqResult.error().message);
  }
  return handleParsedRequest(reqResult.value());
}

} // namespace pp
