#include "server/Server.h"
#include "ledger/network/amp/AmpIdentity.h"
#include "ledger/client/Client.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "ledger/lib/common/Utilities.h"
#include "amp/link/AdpMultiaddr.h"
#include "ledger/network/LedgerRpcProtocol.h"
#include "ledger/chain/ConfigPolicy.h"
#include "common/io/Json.h"

#include <algorithm>
#include <set>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace pp {

// The transport carries the largest block any chain config may allow, with its envelope (B034).
static_assert(ledger::rpc::kMaxMessageBytes >= chain_tx::kMaxBlockBytesCeiling + ledger::rpc::kEnvelopeBytes);
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

/** Resident memory of this process, where the platform tells. */
std::optional<uint64_t> residentBytes() {
#if defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  uint64_t pages = 0;
  uint64_t resident = 0;
  if (statm >> pages >> resident) {
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  }
#endif
  return std::nullopt;
}

uint64_t elapsedMs(RequestQueue::Clock::time_point from, RequestQueue::Clock::time_point to) {
  return to > from ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count())
                   : 0;
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
  if (logFile_ != "none") {
    const std::string path = logFile_.empty() ? workDir + "/" + getLogFileName()
                             : std::filesystem::path(logFile_).is_absolute() ? logFile_
                                                                              : workDir + "/" + logFile_;
    std::lock_guard<std::mutex> lock(handlersMutex);
    const std::string key = log().getFullName() + "|" + path;
    if (handlersAdded.insert(key).second) {
      log().addFileHandler(path, logging::getLevel());
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

std::string Server::packError(uint16_t code, const std::string& message, uint32_t retryAfterMs) {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = code;
  resp.payload = message;
  resp.retryAfterMs = retryAfterMs;
  return utl::binaryPack(resp);
}

void Server::onStop() { stopAmpServer(); }

void Server::fillHealth(Client::NodeHealth& health) const {
  const std::string listen = listenMultiaddr();
  if (const auto at = listen.rfind("/p2p/"); at != std::string::npos) {
    health.peerId = listen.substr(at + 5);
  }
  health.requestQueueDepth = requests_ ? requests_->size() : 0;
  if (const auto tip = latestSignedTip()) {
    health.signedTipHeight = tip->height;
  }
  if (core_) {
    health.networkId = core_->getNetworkId();
    health.nextBlockId = core_->getNextBlockId();
    health.generation = core_->getGeneration();
    health.halted = core_->getEquivocation().has_value();
    health.recoveryPending = core_->getRecoveryStatus().has_value();
  }
  health.ready = false;
  health.notReady = serving_ ? "not ready" : "starting";
  for (const auto& [type, count] : requestCounts_) {
    health.requests.push_back(count);
  }
  health.requestsBusy = requestsBusy_;
  health.requestsRefused = requestsRefused_;
  health.requestsExpired = requestsExpired_;
  health.loopTurnsSlow = loopTurnsSlow_;
  health.loopTurnMaxMs = std::max(turnMaxMs_, lastWindowTurnMaxMs_);
  health.rssBytes = residentBytes();
  {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(minerPeersMutex_);
    health.minerPeers = static_cast<uint64_t>(
        std::count_if(minerPeers_.begin(), minerPeers_.end(), [now](const auto& entry) { return entry.second > now; }));
  }
  if (ampSupport_ && ampSupport_->isRunning()) {
    health.links = ampSupport_->links().CountLinks();
  }
  if (const auto socket = ampSupport_ && ampSupport_->isRunning() ? ampSupport_->host().socketStats() : std::nullopt) {
    health.udpReceiveBufferBytes = socket->receive_buffer_bytes;
    health.udpReceiveDrops = socket->receive_drops;
  }
}

void Server::noteLoopTurn(RequestQueue::Clock::time_point now) {
  constexpr auto kWindow = std::chrono::seconds(10);
  if (now - turnWindowStart_ >= kWindow) {
    lastWindowTurnMaxMs_ = now - turnWindowStart_ < 2 * kWindow ? turnMaxMs_ : 0;
    turnMaxMs_ = 0;
    turnWindowStart_ = now;
  }
  if (lastTurnAt_) {
    const uint64_t turnMs = elapsedMs(*lastTurnAt_, now);
    turnMaxMs_ = std::max(turnMaxMs_, turnMs);
    if (turnMs > static_cast<uint64_t>(Client::NodeHealth::kSlowTurnMs)) {
      ++loopTurnsSlow_;
    }
  }
  lastTurnAt_ = now;
}

void Server::publishHealth() {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(healthMutex_);
    if (health_ && now - healthPublishedAt_ < std::chrono::seconds(1)) {
      return;
    }
  }
  Client::NodeHealth health;
  fillHealth(health);
  std::lock_guard<std::mutex> lock(healthMutex_);
  health_ = std::move(health);
  healthPublishedAt_ = now;
}

std::string Server::healthResponse() const {
  Client::NodeHealth health;
  {
    std::lock_guard<std::mutex> lock(healthMutex_);
    if (!health_) {
      return packResponse(Client::E_SERVER_ERROR, "Health not published yet: starting");
    }
    health = *health_;
    health.reportAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - healthPublishedAt_)
                             .count();
  }
  return packResponse(utl::binaryPack(health.ltsToMeta()));
}

void Server::writeListenMultiaddrFile() const {
  if (workDir_.empty()) {
    return;
  }
  // A wildcard bind is reached on loopback from the same host (or container).
  std::string local = listenMultiaddr();
  for (const auto& [any, loopback] : {std::pair<std::string, std::string>{"/ip4/0.0.0.0/", "/ip4/127.0.0.1/"},
                                      std::pair<std::string, std::string>{"/ip6/::/", "/ip6/::1/"}}) {
    if (local.rfind(any, 0) == 0) {
      local = loopback + local.substr(any.size());
    }
  }
  const auto path = std::filesystem::path(workDir_) / "listen.multiaddr";
  std::ofstream out(path, std::ios::trunc);
  out << local << "\n";
  if (!out) {
    log().warning << "Cannot write " << path.string();
  }
}

void Server::enqueueRequest(std::string body, RequestQueue::Reply reply, std::string peerId) {
  // A reply the transport cannot carry is an error to the client, not a silent timeout (B034).
  reply = [reply = std::move(reply)](std::string response) {
    if (response.size() > ledger::rpc::kMaxMessageBytes) {
      response = packResponse(Client::E_SERVER_ERROR, "Reply of " + std::to_string(response.size()) +
                                                          " bytes is larger than the RPC carries");
    }
    reply(std::move(response));
  };
  const auto type = Client::peekRequestType(body);
  // Only blocks and records come large (B034); anything else above the small cap is refused unread.
  const bool carriesRecord = type == Client::T_REQ_BLOCK_ADD || type == Client::T_REQ_TX_ADD;
  if (!carriesRecord && body.size() > ledger::rpc::kMaxSmallRequestBytes) {
    ++requestsRefused_;
    reply(packResponse(1, "Request of " + std::to_string(body.size()) + " bytes is too large for its type"));
    return;
  }
  if (type == Client::T_REQ_HEALTH) {
    reply(healthResponse()); // a probe: any peer, never queued
    return;
  }
  if (!isAllowedPeer(peerId)) {
    ++requestsRefused_;
    reply(packResponse(1, "Peer not allowed by this node"));
    return;
  }
  // Malformed: refused when served.
  const auto lane = isTrustedPeer(peerId) ? RequestQueue::Lane::Trusted
                    : type                ? laneFor(*type)
                                          : RequestQueue::Lane::Normal;
  if (!requests_->push(std::move(peerId), std::move(body), reply, lane)) {
    ++requestsBusy_;
    reply(packError(Client::E_BUSY, "Server busy, please retry", kBusyRetryAfterMs));
  }
}

std::string Server::refusalResponse(const network::AmpLedgerServer::Refusal why) {
  using Refusal = network::AmpLedgerServer::Refusal;
  switch (why) {
  case Refusal::NotAllowed:
    ++requestsRefused_;
    return packResponse(1, "Peer not allowed by this node");
  case Refusal::PeerBusy:
    ++requestsBusy_;
    return packError(Client::E_RATE_LIMITED, "Too many requests in progress from this peer, please retry",
                     kBusyRetryAfterMs);
  case Refusal::NodeBusy:
    break;
  }
  ++requestsBusy_;
  return packError(Client::E_BUSY, "Too many large requests in progress, please retry", kBusyRetryAfterMs);
}

bool Server::isTrustedPeer(const std::string& peerId) const {
  if (peerId.empty()) {
    return false;
  }
  if (allowedPeers_ && allowedPeers_->contains(peerId)) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(upstreamMutex_);
    if (upstreamPeerIds_.contains(peerId)) {
      return true;
    }
  }
  std::lock_guard<std::mutex> lock(minerPeersMutex_);
  const auto it = minerPeers_.find(peerId);
  return it != minerPeers_.end() && it->second > std::chrono::steady_clock::now();
}

void Server::noteMinerPeer(uint64_t minerId, const std::string& peerId) {
  if (peerId.empty()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(minerPeersMutex_);
  std::erase_if(minerPeers_, [now](const auto& entry) { return entry.second <= now; });
  std::erase_if(minerPeerOf_, [this](const auto& entry) { return !minerPeers_.contains(entry.second); });
  if (auto it = minerPeerOf_.find(minerId); it != minerPeerOf_.end() && it->second != peerId) {
    const std::string moved = it->second;
    minerPeerOf_.erase(it);
    // The miner moved: its old peer stays trusted only for another miner that names it.
    if (std::none_of(minerPeerOf_.begin(), minerPeerOf_.end(), [&](const auto &entry) { return entry.second == moved; })) {
      minerPeers_.erase(moved);
    }
  }
  minerPeerOf_[minerId] = peerId;
  minerPeers_[peerId] = now + kMinerPeerTtl;
}


bool Server::isAllowedPeer(const std::string& peerId) const {
  if (!allowedPeers_ || allowedPeers_->contains(peerId)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(upstreamMutex_);
  return !peerId.empty() && upstreamPeerIds_.contains(peerId);
}

Service::Roe<pp::common::Object> Server::loadRunConfig(const std::vector<env_config::Field>& fields,
                                                     const pp::common::Object& templateJson,
                                                     const std::string& whatToSet) {
  const std::string path = (std::filesystem::path(workDir_) / "config.json").string();
  auto loaded = env_config::loadConfig(path, fields);
  if (!loaded) {
    return Error(E_CONFIG, loaded.error().message);
  }
  if (loaded.value()) {
    return std::move(*loaded.value());
  }
  log().info << "No config.json found, writing a template";
  auto encoded = pp::common::io::valueToJsonString(
      pp::common::Value(std::make_shared<pp::common::Object>(templateJson)), 2);
  if (!encoded.isOk()) {
    return Error(E_CONFIG, "Failed to encode config.json: " + encoded.error().message);
  }
  std::ofstream out(path);
  out << encoded.value() << std::endl;
  if (!out) {
    return Error(E_CONFIG, "Failed to create " + path);
  }
  return Error(E_CONFIG, "Created " + path + ": " + whatToSet);
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
  case Client::T_REQ_CONFIG_GET:
  case Client::T_REQ_TX_GET_BY_WALLET:
  case Client::T_REQ_TX_GET_BY_INDEX:
  case Client::T_REQ_PRODUCER_STATS:
  case Client::T_REQ_ACCOUNT_PROOF:
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

std::string Server::localPeerId() const {
  auto self = upstreamPeerId(listenMultiaddr());
  return self ? self.value() : std::string{};
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

void Server::bindCore(NodeCore& core) {
  core_ = &core;
  handleChainReads();
}

void Server::handle(uint32_t type, Handler handler) { routes_[type] = Route{std::move(handler), {}}; }

void Server::handleLater(uint32_t type, LaterHandler handler) { routes_[type] = Route{{}, std::move(handler)}; }

Service::Roe<void> Server::admit(const Client::Request& /*request*/) const { return {}; }

void Server::handleChainReads() {
  handle(Client::T_REQ_BLOCK_GET, [this](const Client::Request& request) -> Roe<std::string> {
    auto blockId = utl::binaryUnpack<uint64_t>(request.payload);
    if (!blockId) {
      return Error(E_REQUEST, "Invalid block get payload: " + request.payload);
    }
    auto block = core().readBlock(blockId.value());
    if (!block) {
      return Error(E_REQUEST, "Failed to get block: " + block.error().message);
    }
    return block.value().ltsToString();
  });
  handle(Client::T_REQ_ACCOUNT_GET, [this](const Client::Request& request) -> Roe<std::string> {
    auto accountId = utl::binaryUnpack<uint64_t>(request.payload);
    if (!accountId) {
      return Error(E_REQUEST, "Invalid account get payload: " + request.payload);
    }
    auto account = core().getAccount(accountId.value());
    if (!account) {
      return Error(E_REQUEST, "Failed to get account: " + account.error().message);
    }
    return account.value().ltsToString();
  });
  handle(Client::T_REQ_CONFIG_GET, [this](const Client::Request&) -> Roe<std::string> {
    auto meta = core().genesisMetaInForce();
    if (!meta) {
      return Error(E_REQUEST, "Failed to get the config: " + meta.error().message);
    }
    return meta.value().ltsToString();
  });
  handle(Client::T_REQ_TX_GET_BY_WALLET, [this](const Client::Request& request) -> Roe<std::string> {
    auto req = utl::binaryUnpack<Client::TxGetByWalletRequest>(request.payload);
    if (!req) {
      return Error(E_REQUEST, "Failed to deserialize request: " + req.error().message);
    }
    auto found = core().findTransactionsByWalletId(req.value().walletId, req.value().beforeBlockId);
    if (!found) {
      return Error(E_REQUEST, "Failed to get transactions: " + found.error().message);
    }
    Client::TxGetByWalletResponse response;
    response.transactions = found.value();
    response.nextBlockId = req.value().beforeBlockId;
    return utl::binaryPack(response);
  });
  handle(Client::T_REQ_ACCOUNT_PROOF, [this](const Client::Request& request) -> Roe<std::string> {
    auto accountId = utl::binaryUnpack<uint64_t>(request.payload);
    if (!accountId) {
      return Error(E_REQUEST, "Failed to deserialize request: " + accountId.error().message);
    }
    auto proof = core().accountProof(accountId.value());
    if (!proof) {
      return Error(E_REQUEST, "Cannot prove the account: " + proof.error().message);
    }
    return utl::binaryPack(proof.value());
  });
  handle(Client::T_REQ_DOMAIN_GET, [this](const Client::Request& request) -> Roe<std::string> {
    auto domain = utl::binaryUnpack<std::string>(request.payload);
    if (!domain) {
      return Error(E_REQUEST, "Failed to deserialize request: " + domain.error().message);
    }
    auto proof = core().domainProof(domain.value());
    if (!proof) {
      return Error(E_REQUEST, "Cannot prove the domain: " + proof.error().message);
    }
    return utl::binaryPack(proof.value());
  });
  handle(Client::T_REQ_PRODUCER_STATS, [this](const Client::Request& request) -> Roe<std::string> {
    auto req = utl::binaryUnpack<Client::ProducerStatsRequest>(request.payload);
    if (!req) {
      return Error(E_REQUEST, "Failed to deserialize request: " + req.error().message);
    }
    auto stats = core().producerStats(req.value().fromBlockId, req.value().toBlockId);
    if (!stats) {
      return Error(E_REQUEST, "Failed to count producers: " + stats.error().message);
    }
    return utl::binaryPack(stats.value());
  });
  handle(Client::T_REQ_TX_GET_BY_INDEX, [this](const Client::Request& request) -> Roe<std::string> {
    auto req = utl::binaryUnpack<Client::TxGetByIndexRequest>(request.payload);
    if (!req) {
      return Error(E_REQUEST, "Failed to deserialize request: " + req.error().message);
    }
    auto found = core().findTransactionByIndex(req.value().txIndex);
    if (!found) {
      return Error(E_REQUEST, "Failed to get transaction: " + found.error().message);
    }
    return utl::binaryPack(found.value());
  });
  handle(Client::T_REQ_AUTHORITY_PROOF, [this](const Client::Request& request) -> Roe<std::string> {
    auto upTo = utl::binaryUnpack<uint64_t>(request.payload);
    if (!upTo) {
      return Error(E_REQUEST, "Invalid authority proof payload");
    }
    auto proof = core().authorityProof(upTo.value());
    if (!proof) {
      return Error(E_REQUEST, "Failed to build authority proof: " + proof.error().message);
    }
    return utl::binaryPack(proof.value());
  });
  handle(Client::T_REQ_CALIBRATION, [this](const Client::Request&) { return calibrationReply(); });
}

Service::Roe<std::string> Server::calibrationReply() const {
  Client::CalibrationResponse response;
  response.msTimestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count() +
      clockOffsetMs();
  response.nextBlockId = core().getNextBlockId();
  return utl::binaryPack(response);
}

void Server::orderRewindIfDue() {
  if (restartRequested()) {
    return;
  }
  if (auto due = core().scheduleRewind(approvedRewindGeneration()); !due) {
    log().error << due.error().message;
  } else if (due.value()) {
    requestRestart(kRewindRestartGrace); // a forward in flight (the certificate itself) finishes
  }
}

void Server::serveRequestsFor(std::chrono::milliseconds budget) {
  serving_ = true;
  noteLoopTurn(RequestQueue::Clock::now());
  publishHealth();
  auto deadline = RequestQueue::Clock::now() + budget;
  if (restartRequested_) {
    if (RequestQueue::Clock::now() >= restartAt_) {
      setStop(true);
      return;
    }
    deadline = std::min(deadline, restartAt_);
  }
  bool servedOne = false;
  while (!isStopSet()) {
    pollBlockWaits(); // the last item may have moved the tip
    if (servedOne && RequestQueue::Clock::now() >= deadline) {
      return; // the budget is spent: the role's duties get their turn even with requests queued
    }
    servedOne = true;
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
      ++requestsExpired_;
      item->reply(packError(Client::E_EXPIRED, "Request expired in server queue"));
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
    publishHealth(); // probes see a node that is starting, not a dead one
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
  auto deadline = RequestQueue::Clock::now() + timeout;
  auto remaining = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - RequestQueue::Clock::now());
  };
  // Blocks coming in keep it going: the deadline is `timeout` past the last one processed.
  uint64_t seen = sync.blocksProcessed();
  auto noteProgress = [&]() {
    if (const uint64_t now = sync.blocksProcessed(); now != seen) {
      seen = now;
      deadline = RequestQueue::Clock::now() + timeout;
    }
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
      noteProgress();
      continue;
    }
    do {
      serveTasksUntil([&]() { return !sync.inFlight(); }, std::min(remaining(), std::chrono::milliseconds(1000)));
      noteProgress();
    } while (sync.inFlight() && remaining().count() > 0);
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
    ++requestsRefused_;
    item.reply(packResponse(1, request.error().message));
    return;
  }
  // The client's deadline (LR006): a request it gave up on is not served; a handler that forwards
  // it gets what remains.
  if (request.value().deadlineMs > 0) {
    const uint64_t waited = elapsedMs(item.enqueuedAt, RequestQueue::Clock::now());
    if (waited >= request.value().deadlineMs) {
      ++requestsExpired_;
      item.reply(packError(Client::E_EXPIRED, "Request expired in server queue"));
      return;
    }
    request.value().deadlineMs -= static_cast<uint32_t>(waited);
  }
  request.value().from = item.peerId;
  const uint32_t type = request.value().type;
  if (!isAllowedFrom(type, originOf(item.peerId))) {
    ++requestsRefused_;
    log().warning << "Refused request type " << type << " from upstream " << item.peerId;
    item.reply(packResponse(1, "Request type " + std::to_string(type) + " is not accepted from this node's upstream"));
    return;
  }
  const auto started = RequestQueue::Clock::now();
  auto count = [&]() {
    auto& c = requestCounts_[type];
    c.type = type;
    ++c.served;
    c.waitMs += elapsedMs(item.enqueuedAt, started);
    c.serviceMs += elapsedMs(started, RequestQueue::Clock::now());
  };
  if (type == Client::T_REQ_BLOCK_WAIT) {
    serveBlockWait(request.value(), item);
    count();
    return;
  }
  const auto route = routes_.find(type);
  if (route == routes_.end()) {
    ++requestsRefused_;
    item.reply(packResponse(1, "Unsupported request type: " + std::to_string(type)));
    return;
  }
  if (auto admitted = admit(request.value()); !admitted) {
    item.reply(packResponse(1, admitted.error().message));
    count();
    return;
  }
  try {
    if (route->second.later) {
      route->second.later(request.value(), item.reply);
    } else {
      replyWith(item.reply, route->second.now(request.value()));
    }
  } catch (const std::exception& e) {
    log().error << "Request handler threw: " << e.what();
    item.reply(packResponse(Client::E_SERVER_ERROR, "Internal server error"));
  }
  count();
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
    reply(packError(Client::E_BUSY, "Server stopping", kBusyRetryAfterMs));
  }
  for (auto& item : requests_->close()) {
    if (item.reply) {
      item.reply(packError(Client::E_BUSY, "Server stopping", kBusyRetryAfterMs));
    }
  }
}

std::string Server::listenMultiaddr() const {
  return ampSupport_ ? ampSupport_->listenMultiaddr() : std::string{};
}

network::AmpHost* Server::ampRuntime() {
  return ampSupport_ && ampSupport_->isRunning() ? &ampSupport_->host() : nullptr;
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

Service::Roe<void> Server::startAmpServer(const uint16_t port, const IdentityKeyFn& identityKey) {
  if (ampSupport_) {
    return Service::Error(-1, "AMP server already started");
  }
  if (requests_->isClosed()) {
    requests_ = std::make_unique<RequestQueue>(requestCapacity_);  // restart after a stop
  }
  network::AmpLedgerServer::Options ingress;
  ingress.admit = [this](const std::string& peerId) { return isAllowedPeer(peerId); };
  ingress.trusted = [this](const std::string& peerId) { return isTrustedPeer(peerId); };
  ingress.refusal = [this](const network::AmpLedgerServer::Refusal why) { return refusalResponse(why); };
  auto dispatch = [this](std::string peerId, std::string body, RequestQueue::Reply reply) {
    enqueueRequest(std::move(body), std::move(reply), std::move(peerId));
  };
  ampSupport_ = std::make_unique<network::ServerAmpSupport>();
  Service::Roe<void> started;
  if (onHost()) {
    started = ampSupport_->StartHosted(hostAmp_, tuning_.channelReadTimeout(), dispatch, std::move(ingress));
  } else {
    started = startOwnAmp(port, identityKey, dispatch, std::move(ingress));
  }
  if (!started) {
    ampSupport_.reset();
    return started;
  }
  log().info << "AMP ledger listener: " << ampSupport_->listenMultiaddr()
             << (onHost() ? " (the host's runtime)" : "");
  writeListenMultiaddrFile();
  publishHealth(); // HEALTH answers "starting" from now on
  if (onListening_) {
    onListening_(true);
  }
  return {};
}

Service::Roe<void> Server::startOwnAmp(const uint16_t port, const IdentityKeyFn& identityKey,
                                       network::ServerAmpSupport::DispatchFn dispatch,
                                       network::AmpLedgerServer::Options ingress) {
  auto key = identityKey();
  if (!key) {
    return key.error();
  }
  auto config = network::LedgerAmpConfigFromPrivateKey(key.value(), port);
  if (!config) {
    return Service::Error(-1, "Failed to build AMP config: " + config.error().message);
  }
  network::LedgerAmpConfig tuned = std::move(config.value());
  tuning_.applyTo(tuned.link_config);
  tuned.link_config.max_links = maxLinks_;
  tuned.link_config.max_pending_inbound = handshakeBacklog_;
  tuned.udp_buffer_bytes = udpBufferBytes_;
  tuned.rpc_read_timeout = tuning_.channelReadTimeout();
  if (auto started = ampSupport_->Start(std::move(tuned), std::move(dispatch), std::move(ingress)); !started) {
    return started;
  }
  auto* runtime = ampSupport_->ownRuntime();
  if (const auto socket = runtime->socketStats()) {
    log().info << "UDP socket buffers: receive " << socket->receive_buffer_bytes << " B, send "
               << socket->send_buffer_bytes << " B";
  }
  if (runtime->socketBuffersCapped()) {
    log().warning << "UDP socket buffers are smaller than the " << udpBufferBytes_
                  << " B asked for (udpBufferBytes): the OS caps them. Raise the cap, e.g. sysctl -w "
                     "net.core.rmem_max=" << udpBufferBytes_ << " net.core.wmem_max=" << udpBufferBytes_
                  << " — a burst of large requests may otherwise make it drop every peer's datagrams";
  }
  return {};
}

void Server::stopAmpServer() {
  if (ampSupport_ && onListening_) {
    onListening_(false);  // before the protocols go, so a host withdraws them first
  }
  closeRequestQueue();
  if (ampSupport_) {
    ampSupport_->Stop();
    ampSupport_.reset();
  }
}

} // namespace pp
