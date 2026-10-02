#include "Server.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"

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

} // namespace

Server::Server() : requests_(std::make_unique<RequestQueue>(requestCapacity_)) {}

Server::~Server() { stopAmpServer(); }

void Server::setNetworkTuning(const network::NetworkTuning& tuning) {
  tuning_ = tuning;
  setRequestLimits(tuning.requestQueueCapacity, tuning.serverQueueExpiry());
}

void Server::setRequestLimits(size_t capacity, std::chrono::milliseconds maxWait) {
  requestCapacity_ = capacity;
  maxRequestWait_ = maxWait;
  requests_ = std::make_unique<RequestQueue>(requestCapacity_);
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

void Server::onStop() {
  stopAmpServer();
}

void Server::enqueueRequest(std::string body, RequestQueue::Reply reply) {
  if (!requests_->push(std::move(body), reply)) {
    reply(packResponse(Client::E_SERVER_ERROR, "Server busy, please retry"));
  }
}

void Server::postToServerThread(std::function<void()> task) {
  if (!requests_->pushTask(std::move(task))) {
    log().debug << "Dropped completion task: server stopping";
  }
}

bool Server::handleDeferred(const Client::Request& /*request*/, const RequestQueue::Reply& /*reply*/) {
  return false;
}

void Server::serveRequestsFor(std::chrono::milliseconds budget) {
  const auto deadline = RequestQueue::Clock::now() + budget;
  while (!isStopSet()) {
    auto item = requests_->popUntil(deadline);
    if (!item) {
      return;
    }
    if (item->task) {
      try {
        item->task();
      } catch (const std::exception& e) {
        log().error << "Completion task threw: " << e.what();
      }
      continue;
    }
    if (RequestQueue::Clock::now() - item->enqueuedAt > maxRequestWait_) {
      item->reply(packResponse(Client::E_SERVER_ERROR, "Request expired in server queue"));
      continue;
    }
    serveRequest(*item);
  }
}

bool Server::serveTasksUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
  // No isStopSet() check: Service::run() clears the stop flag only after
  // onStart(), which is where this runs. The timeout bounds the wait.
  const auto deadline = RequestQueue::Clock::now() + timeout;
  while (!done()) {
    auto item = requests_->popTaskUntil(deadline);
    if (!item) {
      break;
    }
    try {
      item->task();
    } catch (const std::exception& e) {
      log().error << "Completion task threw: " << e.what();
    }
  }
  return done();
}

std::string Server::runStartupSync(BlockSync& sync, const std::optional<BlockSync::Result>& lastResult,
                                   std::chrono::milliseconds timeout, const std::function<bool()>& extraDone) {
  const auto deadline = RequestQueue::Clock::now() + timeout;
  auto remaining = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - RequestQueue::Clock::now());
  };
  while (remaining().count() > 0) {
    if (sync.start() == BlockSync::Start::BackingOff) {
      serveTasksUntil([]() { return false; }, std::min(remaining(), std::chrono::milliseconds(100)));
      continue;
    }
    serveTasksUntil([&]() { return !sync.inFlight(); }, remaining());
    if (lastResult && lastResult->ok) {
      if (extraDone) {
        serveTasksUntil(extraDone, remaining());
      }
      return {};
    }
    if (lastResult) {
      log().warning << "Startup sync attempt failed, retrying: " << lastResult->error;
    }
  }
  return lastResult ? lastResult->error : std::string("timed out");
}

void Server::serveRequest(const RequestQueue::Item& item) {
  log().debug << "Received request (" << item.body.size() << " bytes)";
  auto request = utl::binaryUnpack<Client::Request>(item.body);
  if (!request) {
    item.reply(packResponse(1, request.error().message));
    return;
  }
  try {
    if (!handleDeferred(request.value(), item.reply)) {
      item.reply(handleParsedRequest(request.value()));
    }
  } catch (const std::exception& e) {
    log().error << "Request handler threw: " << e.what();
    item.reply(packResponse(Client::E_SERVER_ERROR, "Internal server error"));
  }
}

void Server::closeRequestQueue() {
  for (auto& item : requests_->close()) {
    if (item.reply) {
      item.reply(packResponse(Client::E_SERVER_ERROR, "Server stopping"));
    }
  }
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
  if (requests_->isClosed()) {
    requests_ = std::make_unique<RequestQueue>(requestCapacity_);  // restart after a stop
  }
  network::LedgerAmpConfig tuned = config;
  tuning_.applyTo(tuned.link_config);
  ampSupport_ = std::make_unique<network::ServerAmpSupport>();
  auto started = ampSupport_->Start(tuned, [this](std::string body, RequestQueue::Reply reply) {
    enqueueRequest(std::move(body), std::move(reply));
  });
  if (!started) {
    ampSupport_.reset();
    return started;
  }
  log().info << "AMP ledger listener: " << ampSupport_->listenMultiaddr();
  return {};
}

void Server::stopAmpServer() {
  closeRequestQueue();
  if (ampSupport_) {
    ampSupport_->Stop();
    ampSupport_.reset();
  }
}

} // namespace pp
