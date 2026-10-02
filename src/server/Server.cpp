#include "Server.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include "amp/link/AdpMultiaddr.h"

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
  blockWaits_ = BlockWaitList(requestCapacity_);
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

void Server::enqueueRequest(std::string body, RequestQueue::Reply reply, std::string peerId) {
  const auto type = Client::peekRequestType(body);
  const auto lane = type ? laneFor(*type) : RequestQueue::Lane::Normal; // malformed: refused when served
  if (!requests_->push(std::move(peerId), std::move(body), reply, lane)) {
    reply(packResponse(Client::E_SERVER_ERROR, "Server busy, please retry"));
  }
}

bool Server::isAllowedFrom(const uint32_t type, const Origin origin) {
  switch (type) {
  // Writes that only travel up the tree toward the beacon: this node's
  // upstream never sends them down, so one from there is misuse.
  case Client::T_REQ_BLOCK_ADD:
  case Client::T_REQ_REGISTER:
  case Client::T_REQ_TX_ADD:
  // Blocks flow down: our upstream never waits on us for them.
  case Client::T_REQ_BLOCK_WAIT:
  // The registry and the transaction pool live upstream: our upstream never
  // asks us for them.
  case Client::T_REQ_MINER_LIST:
  case Client::T_REQ_TX_PULL:
    return origin == Origin::Downstream;
  default:
    return true;
  }
}

RequestQueue::Lane Server::laneFor(const uint32_t type) {
  switch (type) {
  case Client::T_REQ_BLOCK_GET:
  case Client::T_REQ_BLOCK_WAIT:
  case Client::T_REQ_ACCOUNT_GET:
  case Client::T_REQ_TX_GET_BY_WALLET:
  case Client::T_REQ_TX_GET_BY_INDEX:
  case Client::T_REQ_DOMAIN_GET:
  case Client::T_REQ_NAME_GET:
  case Client::T_REQ_NAME_GET_BY_WALLET:
    return RequestQueue::Lane::Low;
  default:
    return RequestQueue::Lane::Normal;
  }
}

Service::Roe<void> Server::setUpstreams(const std::vector<std::string>& multiaddrs) {
  upstreamPeerIds_.clear();
  for (const auto& multiaddr : multiaddrs) {
    auto parsed = pp::amp::ParseAdpMultiaddr(multiaddr);
    if (!parsed) {
      return Service::Error(-1, "Invalid upstream multiaddr '" + multiaddr + "': " + parsed.error().message);
    }
    upstreamPeerIds_.insert(parsed->peer_id);
  }
  return {};
}

Server::Origin Server::originOf(const std::string& peerId) const {
  return !peerId.empty() && upstreamPeerIds_.contains(peerId) ? Origin::Upstream : Origin::Downstream;
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
    pollBlockWaits(); // the last item may have moved the tip
    auto item = requests_->popUntil(deadline);
    if (!item) {
      pollBlockWaits(); // holds that ended while idle
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
  if (!isAllowedFrom(request.value().type, originOf(item.peerId))) {
    log().warning << "Refused request type " << request.value().type << " from upstream " << item.peerId;
    item.reply(packResponse(1, "Request type " + std::to_string(request.value().type) +
                                   " is not accepted from this node's upstream"));
    return;
  }
  if (request.value().type == Client::T_REQ_BLOCK_WAIT) {
    serveBlockWait(request.value(), item);
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

void Server::serveBlockWait(const Client::Request& request, const RequestQueue::Item& item) {
  const auto tip = blockWaitTip();
  if (!tip) {
    item.reply(packResponse(1, "BLOCK_WAIT is not served by this node"));
    return;
  }
  auto known = utl::binaryUnpack<uint64_t>(request.payload);
  if (!known) {
    item.reply(packResponse(1, "Invalid block wait payload"));
    return;
  }
  const auto deadline = RequestQueue::Clock::now() + tuning_.blockWaitHold();
  answerBlockWaits(blockWaits_.park(item.peerId, known.value(), deadline, item.reply, *tip), *tip);
}

void Server::pollBlockWaits() {
  if (blockWaits_.size() == 0) {
    return;
  }
  const auto tip = blockWaitTip();
  if (!tip) {
    return;
  }
  const auto now = RequestQueue::Clock::now();
  if (*tip == lastPolledTip_ && now < nextBlockWaitSweep_) {
    return;
  }
  lastPolledTip_ = *tip;
  nextBlockWaitSweep_ = now + std::chrono::milliseconds(100);
  answerBlockWaits(blockWaits_.poll(*tip, now), *tip);
}

void Server::answerBlockWaits(std::vector<RequestQueue::Reply> replies, uint64_t tip) {
  const std::string response = packResponse(utl::binaryPack(tip));
  for (auto& reply : replies) {
    reply(response);
  }
}

void Server::closeRequestQueue() {
  for (auto& reply : blockWaits_.takeAll()) {
    reply(packResponse(Client::E_SERVER_ERROR, "Server stopping"));
  }
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
  tuned.rpc_read_timeout = tuning_.channelReadTimeout();
  ampSupport_ = std::make_unique<network::ServerAmpSupport>();
  auto started = ampSupport_->Start(tuned, [this](std::string peerId, std::string body, RequestQueue::Reply reply) {
    enqueueRequest(std::move(body), std::move(reply), std::move(peerId));
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
