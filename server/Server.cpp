#include "Server.h"
#include "../client/Client.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <filesystem>

namespace pp {

Service::Roe<void> Server::run(const std::string& workDir) {
  workDir_ = workDir;

  if (useSignatureFile()) {
    std::filesystem::path signaturePath = std::filesystem::path(workDir) / getFileSignature();
    if (!std::filesystem::exists(workDir)) {
      std::filesystem::create_directories(workDir);
      auto result = utl::writeToNewFile(signaturePath.string(), "");
      if (!result) {
        return Service::Error(getRunErrorCode(),
                             "Failed to create signature file: " + result.error().message);
      }
    }
    if (!std::filesystem::exists(signaturePath)) {
      return Service::Error(getRunErrorCode(),
                            "Work directory not recognized, please remove it manually and try again");
    }
  }

  log().info << "Running " << getServerName() << " with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + getFileLog(), logging::getLevel());

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

void Server::enqueueRequest(QueuedRequest qr) {
  requestQueue_.push(std::move(qr));
}

size_t Server::getRequestQueueSize() const {
  return requestQueue_.size();
}

bool Server::pollAndProcessOneRequest() {
  QueuedRequest qr;
  if (!requestQueue_.poll(qr)) {
    return false;
  }
  processQueuedRequest(qr);
  return true;
}

void Server::processQueuedRequest(QueuedRequest& qr) {
  log().debug << "Processing request from queue";
  std::string response = handleRequest(qr.request);
  sendResponse(qr.fd, response);
}

Service::Roe<void> Server::startFetchServer(const network::TcpEndpoint& endpoint) {
  fetchServer_.redirectLogger(log().getFullName() + ".FetchServer");
  network::FetchServer::Config config;
  config.endpoint = endpoint;
  config.handler = [this](int fd, const std::string& request, const network::TcpEndpoint&) {
    enqueueRequest(QueuedRequest{ fd, request });
    log().debug << "Request enqueued (queue size: " << getRequestQueueSize() << ")";
  };
  return fetchServer_.start(config);
}

void Server::stopFetchServer() {
  fetchServer_.stop();
}

void Server::onStop() {
  stopFetchServer();
}

void Server::sendResponse(int fd, const std::string& response) {
  auto addResponseResult = fetchServer_.addResponse(fd, response);
  if (!addResponseResult) {
    log().error << "Failed to queue response: " << addResponseResult.error().message;
  }
}

std::string Server::handleRequest(const std::string& request) {
  log().debug << "Received request (" << request.size() << " bytes)";
  auto reqResult = utl::binaryUnpack<Client::Request>(request);
  if (!reqResult) {
    return packResponse(1, reqResult.error().message);
  }
  return handleParsedRequest(reqResult.value());
}

} // namespace pp
