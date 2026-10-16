#include "Server.h"
#include "../client/Client.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include "amp/link/AdpMultiaddr.h"

#include <set>
#include <mutex>
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
  // Once per process: a server restarted in-process (a rewind) logs to the same file through the
  // same logger, and the logger cannot drop a handler.
  static std::mutex handlersMutex;
  static std::set<std::string> handlersAdded;
  {
    std::lock_guard<std::mutex> lock(handlersMutex);
    const std::string key = log().getFullName() + "|" + workDir + "/" + getLogFileName();
    if (handlersAdded.insert(key).second) {
      log().addFileHandler(workDir + "/" + getLogFileName(), logging::getLevel());
    }
  }

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
  if (!isAllowedPeer(peerId)) {
    reply(packResponse(1, "Peer not allowed by this node"));
    return;
  }
  const auto type = Client::peekRequestType(body);
  const auto lane = type ? laneFor(*type) : RequestQueue::Lane::Normal; // malformed: refused when served
  if (!requests_->push(std::move(peerId), std::move(body), reply, lane)) {
    reply(packResponse(Client::E_SERVER_ERROR, "Server busy, please retry"));
  }
}

bool Server::isAllowedPeer(const std::string& peerId) const {
  if (!allowedPeers_ || allowedPeers_->contains(peerId)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(upstreamMutex_);
  return !peerId.empty() && upstreamPeerIds_.contains(peerId);
}

Service::Roe<std::optional<std::set<std::string>>> Server::parseAllowedPeers(const pp::common::Object& config) {
  if (!config.contains("allowedPeers")) {
    return std::optional<std::set<std::string>>{};
  }
  const pp::common::Array* list = config.getArray("allowedPeers");
  if (!list) {
    return Service::Error(-1, "Field 'allowedPeers' must be an array of peer ids");
  }
  std::set<std::string> peers;
  for (const auto& value : list->elements) {
    auto id = pp::common::asString(value);
    if (!id || id->empty()) {
      return Service::Error(-1, "Field 'allowedPeers' entries must be peer id strings");
    }
    peers.insert(*id);
  }
  return std::optional<std::set<std::string>>(std::move(peers));
}

bool Server::isAllowedFrom(const uint32_t type, const Origin origin) {
  switch (type) {
  // Writes that only travel up the tree toward the beacon: this node's
  // upstream never sends them down, so one from there is misuse.
  case Client::T_REQ_BLOCK_ADD:
  case Client::T_REQ_REGISTER:
  case Client::T_REQ_TX_ADD:
  // Equivocation evidence travels up to the beacon; down, it rides STATUS.
  case Client::T_REQ_EQUIVOCATION_ADD:
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
  case Client::T_REQ_AUTHORITY_PROOF:
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

namespace {
Service::Roe<std::string> upstreamPeerId(const std::string& multiaddr) {
  auto parsed = pp::amp::ParseAdpMultiaddr(multiaddr);
  if (!parsed) {
    return Service::Error(-1, "Invalid upstream multiaddr '" + multiaddr + "': " + parsed.error().message);
  }
  return parsed->peer_id;
}
} // namespace

Service::Roe<void> Server::setUpstreams(const std::vector<std::string>& multiaddrs) {
  std::set<std::string> peerIds;
  for (const auto& multiaddr : multiaddrs) {
    auto peerId = upstreamPeerId(multiaddr);
    if (!peerId) {
      return peerId.error();
    }
    peerIds.insert(peerId.value());
  }
  std::lock_guard<std::mutex> lock(upstreamMutex_);
  upstreamPeerIds_ = std::move(peerIds);
  followedPeerId_ = multiaddrs.empty() ? std::string{} : upstreamPeerId(multiaddrs.front()).value();
  return {};
}

Service::Roe<void> Server::addUpstream(const std::string& multiaddr) {
  auto peerId = upstreamPeerId(multiaddr);
  if (!peerId) {
    return peerId.error();
  }
  if (isSelf(multiaddr)) {
    return Service::Error(-1, "Upstream '" + multiaddr + "' is this node");
  }
  std::lock_guard<std::mutex> lock(upstreamMutex_);
  upstreamPeerIds_.insert(peerId.value());
  return {};
}

bool Server::isSelf(const std::string& multiaddr) const {
  auto peerId = upstreamPeerId(multiaddr);
  auto self = upstreamPeerId(listenMultiaddr());
  return peerId && self && peerId.value() == self.value();
}

Service::Roe<void> Server::followUpstream(const std::string& multiaddr) {
  auto peerId = upstreamPeerId(multiaddr);
  if (!peerId) {
    return peerId.error();
  }
  std::lock_guard<std::mutex> lock(upstreamMutex_);
  followedPeerId_ = peerId.value();
  return {};
}

UpstreamSet::Hooks Server::upstreamHooks(Client& fanout) {
  UpstreamSet::Hooks hooks;
  hooks.fetchStatus = [this, &fanout](size_t index, const std::string& multiaddr, UpstreamSet::StatusDone done) {
    // Each call captures the dial key set just before it, so the probes run in parallel.
    if (auto dial = fanout.setAmpPeer(UpstreamSet::dialKey(index), multiaddr); !dial) {
      done(Client::Roe<Client::BeaconState>(dial.error()));
      return;
    }
    fanout.fetchBeaconStateAsync(completeOnServerThread<Client::BeaconState>(std::move(done)));
  };
  hooks.adopt = [this](const std::string& multiaddr) {
    auto added = addUpstream(multiaddr);
    if (!added) {
      log().debug << "Not adding dial hint: " << added.error().message;
    }
    return added.isOk();
  };
  hooks.onFollow = [this](const std::string& multiaddr) {
    if (auto followed = followUpstream(multiaddr); !followed) {
      log().warning << followed.error().message;
    }
  };
  return hooks;
}

clock_calibration::Fetch Server::calibrationFetch(Client& client, std::string dialKey, std::string multiaddr) {
  return [this, &client, dialKey = std::move(dialKey), multiaddr = std::move(multiaddr)](
             Client::Done<Client::CalibrationResponse> done) {
    if (auto dial = client.setAmpPeer(dialKey, multiaddr); !dial) {
      done(Client::Roe<Client::CalibrationResponse>(dial.error()));
      return;
    }
    client.fetchCalibrationAsync(completeOnServerThread<Client::CalibrationResponse>(std::move(done)));
  };
}

Client::Roe<clock_calibration::Offset> Server::calibrateBlocking(const clock_calibration::Fetch& fetch) {
  // Shared: a sample still in flight when this gives up must not write into a returned frame.
  auto result = std::make_shared<std::optional<Client::Roe<clock_calibration::Offset>>>();
  clock_calibration::estimate(fetch, [result](Client::Roe<clock_calibration::Offset> offset) {
    *result = std::move(offset);
  });
  // Each sample is bounded by the RPC timeout; a stuck one is given up on.
  const auto timeout = tuning_.rpcTimeout * static_cast<int64_t>(clock_calibration::kMaxSamples + 1);
  serveTasksUntil([&result]() { return result->has_value(); }, timeout);
  if (!result->has_value()) {
    return Client::Error(Client::E_NOT_CONNECTED, "Clock calibration timed out");
  }
  return std::move(**result);
}

Server::Origin Server::originOf(const std::string& peerId) const {
  std::lock_guard<std::mutex> lock(upstreamMutex_);
  return !peerId.empty() && peerId == followedPeerId_ ? Origin::Upstream : Origin::Downstream;
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
  auto deadline = RequestQueue::Clock::now() + budget;
  if (restartRequested_) {
    if (RequestQueue::Clock::now() >= restartAt_) {
      setStop(true);
      return;
    }
    deadline = std::min(deadline, restartAt_);
  }
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
  // Waits in short slices so a stop request (SIGTERM during a startup sync) ends it promptly.
  constexpr auto kSlice = std::chrono::milliseconds(200);
  const auto deadline = RequestQueue::Clock::now() + timeout;
  while (!done() && !isStopSet()) {
    const auto now = RequestQueue::Clock::now();
    if (now >= deadline) {
      break;
    }
    auto item = requests_->popTaskUntil(std::min(deadline, now + kSlice));
    if (!item) {
      if (requests_->isClosed()) {
        break; // nothing more will come
      }
      continue;
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
    if (restartRequested_) {
      return {}; // the node restarts (a rewind) instead of syncing on
    }
    if (isStopSet()) {
      log().info << "Startup sync interrupted: stop requested";
      return {}; // Service::run() sees the stop and does not run
    }
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
  if (replies.empty()) {
    return;
  }
  Client::TipReply answer;
  answer.nextBlockId = tip;
  answer.setTip(latestSignedTip());
  const std::string response = packResponse(utl::binaryPack(answer));
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

Service::Roe<std::string> Server::loadOrCreateIdentityKey() {
  // From the environment (PP_LEDGER_IDENTITY_KEY or _FILE, hex): a stable peer id, nothing written.
  auto fromEnv = env_config::secret("PP_LEDGER_IDENTITY_KEY");
  if (!fromEnv) {
    return Service::Error(-1, fromEnv.error().message);
  }
  if (fromEnv.value()) {
    const std::string key = utl::hexDecode(*fromEnv.value());
    if (key.size() != utl::kMlDsaPrivateKeyBytes) {
      return Service::Error(-1, "PP_LEDGER_IDENTITY_KEY is not a hex ML-DSA-65 private key");
    }
    return key;
  }
  const std::filesystem::path keysDir = std::filesystem::path(workDir_) / "keys";
  const std::filesystem::path keyPath = keysDir / "amp-identity.txt";
  if (!std::filesystem::exists(keyPath)) {
    std::error_code ec;
    std::filesystem::create_directories(keysDir, ec);
    auto generated = utl::mlDsaGenerate();
    if (!generated) {
      return Service::Error(-1, "Failed to generate identity key: " + generated.error().message);
    }
    auto written = utl::writeToNewFile(keyPath.string(), utl::hexEncode(generated->privateKey) + "\n");
    if (!written) {
      return Service::Error(-1, "Failed to write identity key: " + written.error().message);
    }
    log().info << "Created network identity key: " << keyPath.string();
  }
  // Relative to the work dir (readPrivateKey resolves it there): keyPath
  // already includes workDir_, which a relative work dir would double.
  auto key = utl::readPrivateKey("keys/amp-identity.txt", workDir_);
  if (!key) {
    return Service::Error(-1, "Failed to load identity key: " + key.error().message);
  }
  return key.value();
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
