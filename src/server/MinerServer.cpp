#include "MinerServer.h"
#include "TxForwardPolicy.h"
#include "../chain/BlockValidation.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "../network/amp/AmpIdentity.h"
#include "NetworkAnchor.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "common/io/Json.h"
#include <vector>

namespace pp {
namespace {
using pp::common::Array;
using pp::common::Object;
using pp::common::ObjectPtr;
using pp::common::Value;
using pp::common::asNonNegInt;
using pp::common::asObject;
using pp::common::asString;
using pp::common::io::valueToJsonString;

pp::Roe<std::string> encodeObjectPretty(const Object &o) {
  auto r = valueToJsonString(Value(std::make_shared<Object>(o)), 2);
  if (!r.isOk()) {
    return pp::Error(r.error().message);
  }
  return r.value();
}
} // namespace


Object MinerServer::RunFileConfig::ltsToJson() const {
  Object j;
  j.setUIntForJson("minerId", minerId);
  std::vector<Value> keyVals;
  for (const auto &k : keys) {
    keyVals.push_back(k);
  }
  j.set("keys", Object::array(std::move(keyVals)));
  j.set("host", host);
  j.setJsonUInt("port", port);
  std::vector<Value> beaconVals;
  for (const auto &b : beacons) {
    beaconVals.push_back(b);
  }
  j.set("beacons", Object::array(std::move(beaconVals)));
  return j;
}

MinerServer::Roe<void>
MinerServer::RunFileConfig::ltsFromJson(const Object &jd) {
  auto minerIdOpt = jd.getNonNegInt("minerId");
  if (!minerIdOpt) {
    return Error(E_CONFIG, jd.contains("minerId")
                               ? "Field 'minerId' must be a non-negative integer"
                               : "Field 'minerId' is required");
  }
  minerId = *minerIdOpt;

  const Array *keysArr = jd.getArray("keys");
  if (!keysArr) {
    return Error(E_CONFIG, jd.contains("keys") ? "Field 'keys' must be an array"
                                               : "Field 'keys' is required");
  }
  keys.clear();
  for (size_t i = 0; i < keysArr->elements.size(); ++i) {
    auto keyFile = asString(keysArr->elements[i]);
    if (!keyFile) {
      return Error(E_CONFIG,
                   "All elements in 'keys' array must be strings (index " +
                       std::to_string(i) + " is not)");
    }
    if (keyFile->empty()) {
      return Error(E_CONFIG, "Key file at index " + std::to_string(i) +
                                 " cannot be empty");
    }
    keys.push_back(*keyFile);
  }
  if (keys.empty()) {
    return Error(E_CONFIG, "Field 'keys' array must contain at least one key file");
  }

  auto hostOpt = jd.getString("host");
  if (!hostOpt) {
    return Error(E_CONFIG, jd.contains("host") ? "Field 'host' must be a string"
                                               : "Field 'host' is required");
  }
  host = *hostOpt;
  if (host.empty()) {
    return Error(E_CONFIG, "Field 'host' cannot be empty");
  }

  auto portValue = jd.getNonNegInt("port");
  if (!portValue || *portValue == 0 || *portValue > 65535) {
    return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
  }
  port = static_cast<uint16_t>(*portValue);

  const Array *beaconsArr = jd.getArray("beacons");
  if (!beaconsArr) {
    return Error(E_CONFIG, jd.contains("beacons") ? "Field 'beacons' must be an array"
                                                  : "Field 'beacons' is required");
  }
  if (beaconsArr->elements.empty()) {
    return Error(E_CONFIG, "Field 'beacons' array must contain at least one beacon entry");
  }
  beacons.clear();
  for (size_t i = 0; i < beaconsArr->elements.size(); ++i) {
    if (auto s = asString(beaconsArr->elements[i])) {
      beacons.push_back(*s);
      continue;
    }
    const Object *beacon = asObject(beaconsArr->elements[i]);
    if (!beacon) {
      return Error(E_CONFIG,
                   "beacons[] entries must be multiaddr strings or objects (index " +
                       std::to_string(i) + ")");
    }
    auto ma = network::ParseBeaconMultiaddr(*beacon);
    if (!ma) {
      return Error(E_CONFIG, "Failed to parse beacon configuration: " + ma.error().message);
    }
    beacons.push_back(std::move(*ma));
  }
  if (const Object* anchorObj = jd.getObject("networkAnchor")) {
    network_anchor = NetworkAnchor::fromJson(*anchorObj);
  }
  return {};
}

// ============ MinerServer methods ============

MinerServer::MinerServer() {
  redirectLogger("MinerServer");
  miner_.redirectLogger(log().getFullName() + ".Miner");
  client_.redirectLogger(log().getFullName() + ".Client");
  forwardClient_.redirectLogger(log().getFullName() + ".ForwardClient");
}

MinerServer::~MinerServer() {}

Client::Roe<void> MinerServer::dialPeerMultiaddr(const std::string& multiaddr,
                                                 const std::string& peer_key) {
  return client_.setAmpPeer(peer_key, multiaddr);
}

MinerServer::Roe<void> MinerServer::dialUpstreamIndex(size_t index) {
  if (index >= config_.network.beacon_multiaddrs.size()) {
    return Error(E_CONFIG, "upstream index out of range");
  }
  auto dial = dialPeerMultiaddr(config_.network.beacon_multiaddrs[index], "beacon");
  if (!dial) {
    return Error(E_NETWORK, dial.error().message);
  }
  return {};
}

MinerServer::Roe<void> MinerServer::dialActiveUpstream() {
  return dialUpstreamIndex(active_upstream_index_);
}

MinerServer::Roe<void> MinerServer::verifyUpstreamState(const Client::BeaconState& state) {
  return NetworkAnchor::verifyStatus<Error>(config_.network.network_anchor, state);
}

MinerServer::Roe<void> MinerServer::verifyGenesisAnchor() {
  if (config_.network.network_anchor.genesis_hash.empty()) {
    return {};
  }
  auto block = client_.fetchBlock(0);
  if (!block) {
    return Error(E_NETWORK,
                 "Failed to fetch genesis block for anchor verification: " +
                     block.error().message);
  }
  // Recompute the hash from the block body instead of trusting the upstream
  // peer's self-reported `hash` field, which a malicious/compromised
  // upstream could set to match the pinned anchor while serving a forged
  // genesis body.
  const std::string calculatedHash =
      chain_block::calculateBlockHash(block.value().block);
  return NetworkAnchor::verifyGenesisHash<Error>(config_.network.network_anchor,
                                                 calculatedHash);
}

MinerServer::Roe<size_t> MinerServer::selectBestUpstreamIndex() {
  size_t best = config_.network.beacon_multiaddrs.size();
  uint64_t best_height = 0;
  for (size_t i = 0; i < config_.network.beacon_multiaddrs.size(); ++i) {
    if (auto dial = dialUpstreamIndex(i); !dial) {
      log().warning << "Skipping upstream [" << i << "] "
                    << config_.network.beacon_multiaddrs[i] << ": "
                    << dial.error().message;
      continue;
    }
    auto status = client_.fetchBeaconState();
    if (!status) {
      log().warning << "Skipping upstream [" << i << "]: status failed: "
                    << status.error().message;
      continue;
    }
    if (auto verified = verifyUpstreamState(status.value()); !verified) {
      log().warning << "Skipping upstream [" << i << "]: " << verified.error().message;
      continue;
    }
    if (status.value().nextBlockId >= best_height) {
      best_height = status.value().nextBlockId;
      best = i;
    }
  }
  if (best >= config_.network.beacon_multiaddrs.size()) {
    return Error(E_NETWORK, "No healthy upstream available");
  }
  log().info << "Selected upstream [" << best << "] "
             << config_.network.beacon_multiaddrs[best] << " (nextBlockId="
             << best_height << ")";
  return best;
}

Service::Roe<void> MinerServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(getWorkDir()) / FILE_CONFIG;
  std::string configPathStr = configPath.string();

  // Create default FILE_CONFIG if it doesn't exist using RunFileConfig
  RunFileConfig runFileConfig;

  if (!std::filesystem::exists(configPath)) {
    log().info << "No " << FILE_CONFIG
               << " found, creating with default values";

    // Use default values from RunFileConfig struct
    auto encoded = encodeObjectPretty(runFileConfig.ltsToJson());
    if (!encoded) {
      return Service::Error(E_MINER, "Failed to encode " + std::string(FILE_CONFIG) +
                                         ": " + encoded.error().message);
    }
    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_MINER,
                            "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << encoded.value() << std::endl;
    configFile.close();

    log().info << "Created " << FILE_CONFIG << " at: " << configPathStr;
    log().info << "Please edit " << FILE_CONFIG
               << " to configure your miner settings";
  } else {
    // Load existing configuration
    auto jsonResult = utl::loadJsonFile(configPathStr);
    if (!jsonResult) {
      return Service::Error(E_CONFIG, "Failed to load config file: " +
                                          jsonResult.error().message);
    }

    auto parseResult = runFileConfig.ltsFromJson(jsonResult.value());
    if (!parseResult) {
      return Service::Error(E_CONFIG, "Failed to parse config file: " +
                                          parseResult.error().message);
    }
  }

  // Apply configuration from RunFileConfig
  config_.minerId = runFileConfig.minerId;
  std::filesystem::path configDir =
      std::filesystem::path(getWorkDir());
  config_.privateKeys.clear();
  for (const auto &keyFile : runFileConfig.keys) {
    auto keyResult = utl::readPrivateKey(keyFile, configDir.string());
    if (!keyResult) {
      return Service::Error(E_CONFIG,
                            "Failed to load key '" + keyFile + "': " +
                                keyResult.error().message);
    }
    config_.privateKeys.push_back(keyResult.value());
  }
  config_.network.udp_port = runFileConfig.port;
  config_.network.beacon_multiaddrs = runFileConfig.beacons;
  config_.network.network_anchor = runFileConfig.network_anchor;

  log().info << "Configuration loaded";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  UDP port: " << config_.network.udp_port;
  log().info << "  Beacons: " << config_.network.beacon_multiaddrs.size();

  auto ampCfg = network::LedgerAmpConfigFromPrivateKey(config_.privateKeys.front(), config_.network.udp_port);
  if (!ampCfg) {
    return Service::Error(E_CONFIG, "Failed to build AMP config: " + ampCfg.error().message);
  }
  auto serverStarted = startAmpServer(*ampCfg);
  if (!serverStarted) {
    return Service::Error(E_MINER, "Failed to start AMP server: " + serverStarted.error().message);
  }

  if (!ampRuntime()) {
    return Service::Error(E_NETWORK, "AMP runtime unavailable after start");
  }
  client_.attachAmpTransport(*ampRuntime(), "beacon");
  forwardClient_.attachAmpTransport(*ampRuntime(), "leader");

  // Connect to beacon server and fetch initial state
  auto beaconResult = connectToBeacon();
  if (!beaconResult) {
    return Service::Error(E_NETWORK, "Failed to connect to beacon: " +
                                         beaconResult.error().message);
  }
  log().info
      << "Successfully connected to beacon and synchronized initial state";
  const auto &state = beaconResult.value();

  auto calResult = calibrateTimeToBeacon();
  if (!calResult) {
    return Service::Error(E_NETWORK, "Failed to calibrate time to beacon: " +
                                       calResult.error().message);
  }
  timeOffsetToBeaconMs_ = calResult.value();

  // Initialize miner core
  std::filesystem::path minerDataDir =
      std::filesystem::path(getWorkDir()) / DIR_DATA;

  Miner::InitConfig minerConfig;
  minerConfig.minerId = config_.minerId;
  minerConfig.privateKeys = config_.privateKeys;
  minerConfig.timeOffset = timeOffsetToBeaconMs_ / 1000;
  minerConfig.workDir = minerDataDir.string();
  minerConfig.startingBlockId = state.checkpointId;

  auto minerInit = miner_.init(minerConfig);
  if (!minerInit) {
    return Service::Error(E_MINER, "Failed to initialize Miner: " +
                                       minerInit.error().message);
  }

  auto syncResult = syncBlocksFromBeacon();
  if (!syncResult) {
    return Service::Error(E_MINER, "Failed to sync blocks from beacon: " +
                                       syncResult.error().message);
  }

  lastBlockSyncTime_ = std::chrono::steady_clock::now();
  lastSyncedEpoch_ = miner_.getCurrentEpoch();

  refreshMinerListFromBeacon();

  initHandlers();

  log().info << "Miner core initialized";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake at init: " << miner_.getStake();

  log().info << "MinerServer initialization complete";
  return {};
}

MinerServer::Roe<int64_t> MinerServer::calibrateTimeToBeacon() {
  if (config_.network.beacon_multiaddrs.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
  }
  if (auto dial = dialActiveUpstream(); !dial) {
    return Error(E_NETWORK, dial.error().message);
  }

  struct Sample {
    int64_t offsetMs;
    int64_t rttMs;
  };
  std::vector<Sample> samples;
  samples.reserve(static_cast<size_t>(CALIBRATION_SAMPLES));

  for (int i = 0; i < CALIBRATION_SAMPLES; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    auto result = client_.fetchCalibration();
    auto t1 = std::chrono::steady_clock::now();
    if (!result) {
      return Error(E_NETWORK,
                   "Failed to fetch beacon timestamp: " + result.error().message);
    }
    int64_t serverTimeMs = result.value().msTimestamp;
    int64_t localTimeMs = utl::getCurrentTime() * 1000;
    int64_t rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    int64_t offsetMs = serverTimeMs - localTimeMs + (rttMs / 2);
    samples.push_back({offsetMs, rttMs});

    if (rttMs <= RTT_THRESHOLD_MS) {
      log().info << "Time calibrated to beacon: offset=" << offsetMs << " ms, RTT=" << rttMs
                 << " ms (single sample)";
      return offsetMs;
    }
    if (i == 0) {
      log().debug << "High RTT (" << rttMs << " ms), taking up to " << CALIBRATION_SAMPLES
                  << " samples";
    }
  }

  auto best = std::min_element(samples.begin(), samples.end(),
                              [](const Sample &a, const Sample &b) { return a.rttMs < b.rttMs; });
  int64_t offsetMs = best->offsetMs;
  log().info << "Time calibrated to beacon: offset=" << offsetMs << " ms, samples=" << samples.size()
             << ", min RTT=" << best->rttMs << " ms";
  return offsetMs;
}

MinerServer::Roe<void> MinerServer::syncBlocksFromBeacon() {
  if (config_.network.beacon_multiaddrs.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
  }

  if (auto dial = dialActiveUpstream(); !dial) {
    auto best = selectBestUpstreamIndex();
    if (!best) {
      return Error(E_NETWORK, best.error().message);
    }
    active_upstream_index_ = best.value();
    if (auto redial = dialActiveUpstream(); !redial) {
      return Error(E_NETWORK, redial.error().message);
    }
  }

  const auto& upstream_ma = config_.network.beacon_multiaddrs[active_upstream_index_];
  log().info << "Syncing blocks from upstream: " << upstream_ma;

  auto calibrationResult = client_.fetchCalibration();
  if (!calibrationResult) {
    return Error(E_NETWORK,
                 "Failed to get beacon calibration: " + calibrationResult.error().message);
  }

  uint64_t latestBlockId = calibrationResult.value().nextBlockId;
  uint64_t nextBlockId = miner_.getNextBlockId();

  if (nextBlockId >= latestBlockId) {
    log().info << "Already in sync: next block " << nextBlockId
               << ", beacon latest " << latestBlockId;
    return {};
  }

  log().info << "Syncing blocks " << nextBlockId << " to " << latestBlockId;

  for (uint64_t blockId = nextBlockId; blockId < latestBlockId; ++blockId) {
    auto blockResult = client_.fetchBlock(blockId);
    if (!blockResult) {
      return Error(E_NETWORK,
                   "Failed to fetch block " + std::to_string(blockId) +
                       " from beacon: " + blockResult.error().message);
    }

    Ledger::ChainNode block = blockResult.value();
    block.hash = miner_.calculateHash(block.block);

    auto addResult = miner_.addBlock(block);
    if (!addResult) {
      return Error(E_MINER, "Failed to add block " + std::to_string(blockId) +
                                ": " + addResult.error().message);
    }

    log().debug << "Synced block " << blockId;
  }

  log().info << "Sync complete: " << (latestBlockId - nextBlockId)
             << " blocks added";

  return {};
}

void MinerServer::initHandlers() {
  requestHandlers_.clear();

  auto &hgs = requestHandlers_[Client::T_REQ_STATUS];
  hgs = [this](const Client::Request &request) { return hStatus(request); };

  auto &hcs = requestHandlers_[Client::T_REQ_CALIBRATION];
  hcs = [this](const Client::Request &request) { return hCalibration(request); };

  auto &hga = requestHandlers_[Client::T_REQ_ACCOUNT_GET];
  hga = [this](const Client::Request &request) { return hAccountGet(request); };

  auto &htx = requestHandlers_[Client::T_REQ_TX_GET_BY_WALLET];
  htx = [this](const Client::Request &request) { return hTxGetByWallet(request); };

  auto &htxi = requestHandlers_[Client::T_REQ_TX_GET_BY_INDEX];
  htxi = [this](const Client::Request &request) { return hTxGetByIndex(request); };

  auto &hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab = [this](const Client::Request &request) { return hBlockAdd(request); };


  auto &htf = requestHandlers_[Client::T_REQ_TX_FORWARD];
  htf = [this](const Client::Request &request) { return hTxForward(request); };

  // Handlers that need another server (or a sync) reply later; see handleDeferred.
  deferredHandlers_.clear();
  deferredHandlers_[Client::T_REQ_BLOCK_GET] = [this](const Client::Request &r, const RequestQueue::Reply &reply) {
    dBlockGet(r, reply);
  };
  deferredHandlers_[Client::T_REQ_TX_ADD] = [this](const Client::Request &r, const RequestQueue::Reply &reply) {
    dTxAdd(r, reply);
  };
}

bool MinerServer::handleDeferred(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto it = deferredHandlers_.find(request.type);
  if (it == deferredHandlers_.end()) {
    return false;
  }
  it->second(request, reply);
  return true;
}

void MinerServer::onStop() {
  // Reply before AMP stops (Server::onStop), while replies can still be sent.
  for (auto &pending : pendingBlockGets_) {
    pending.reply(packResponse(Client::E_SERVER_ERROR, "Server stopping"));
  }
  pendingBlockGets_.clear();
  Server::onStop();
  log().info << "MinerServer resources cleaned up";
}

void MinerServer::runLoop() {
  log().info << "Block production and request handler loop started";

  while (!isStopSet()) {
    try {
      // Update miner state
      miner_.refresh();

      syncBlocksPeriodically();
      if (blockSyncRequested_) {
        blockSyncRequested_ = false;
        trySyncBlocksFromBeacon(true);
      }
      resolvePendingBlockGets();
      if (minerListRefreshRequested_) {
        minerListRefreshRequested_ = false;
        refreshMinerListFromBeacon();
      }

      if (!miner_.isConfigReady()) {
        // Config not loaded yet (late joiner waiting for T_CONFIG in synced blocks).
        // Skip slot leader / validator roles; continue syncing only.
      } else if (miner_.isSlotLeader()) {
        handleSlotLeaderRole();
      } else {
        handleValidatorRole();
      }

      // Sleep for a short time before checking again
      serveRequestsFor(std::chrono::milliseconds(100));

    } catch (const std::exception &e) {
      log().error << "Exception in block production loop: " << e.what();
      serveRequestsFor(std::chrono::seconds(1));
    }
  }

  log().info << "Block production and request handler loop stopped";
}

void MinerServer::trySyncBlocksFromBeacon(bool bypassRateLimit) {
  const uint64_t slotDurationSec = miner_.getSlotDuration();
  if (!bypassRateLimit && slotDurationSec > 0) {
    auto now = std::chrono::steady_clock::now();
    auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastBlockSyncTime_).count();
    if (elapsedSec < static_cast<int64_t>(slotDurationSec)) {
      return; // Rate limit: at most one sync per slot time
    }
  }
  auto syncResult = syncBlocksFromBeacon();
  if (syncResult) {
    lastBlockSyncTime_ = std::chrono::steady_clock::now();
    lastSyncedEpoch_ = miner_.getCurrentEpoch();
  } else {
    log().warning << "Block sync failed: " << syncResult.error().message;
  }
}

void MinerServer::syncBlocksPeriodically() {
  const uint64_t currentEpoch = miner_.getCurrentEpoch();
  const uint64_t currentSlot = miner_.getCurrentSlot();
  const uint64_t slotDurationSec = miner_.getSlotDuration();
  if (slotDurationSec == 0) {
    return;
  }

  // 1. At beginning of each epoch: sync to update stakeholders
  const bool needSyncForEpoch = (currentEpoch > lastSyncedEpoch_);

  // 2. Before producing: we are expected to be slot leader for next slot; sync before that slot starts
  const uint64_t nextSlot = currentSlot + 1;
  const bool weAreLeaderForNextSlot = miner_.isSlotLeaderForSlot(nextSlot);
  const int64_t nowSec = miner_.getConsensusTimestamp();
  const int64_t nextSlotStartSec = miner_.getSlotStartTime(nextSlot);
  const int64_t secUntilNextSlot = nextSlotStartSec - nowSec;
  const bool needSyncBeforeProduce =
      weAreLeaderForNextSlot && secUntilNextSlot >= 0 &&
      secUntilNextSlot <= SYNC_BEFORE_SLOT_SECONDS;

  if (!needSyncForEpoch && !needSyncBeforeProduce) {
    return;
  }

  trySyncBlocksFromBeacon(false);
}

void MinerServer::refreshMinerListFromBeacon() {
  if (config_.network.beacon_multiaddrs.empty()) {
    return;
  }
  if (auto dial = dialActiveUpstream(); !dial) {
    auto best = selectBestUpstreamIndex();
    if (!best) {
      log().warning << "Failed to select upstream for miner list: "
                    << best.error().message;
      return;
    }
    active_upstream_index_ = best.value();
    if (auto redial = dialActiveUpstream(); !redial) {
      log().warning << "Failed to dial upstream for miner list: "
                    << redial.error().message;
      return;
    }
  }
  auto minerListResult = client_.fetchMinerList();
  if (minerListResult) {
    for (const auto &miner : minerListResult.value()) {
      config_.mMiners[miner.id] = miner;
    }
    lastMinerListFetchTime_ = std::chrono::steady_clock::now();
    log().info << "Fetched miner list: " << config_.mMiners.size()
               << " registered miners";
  } else {
    log().warning << "Failed to fetch miner list: "
                  << minerListResult.error().message;
  }
}

std::string MinerServer::lookupTxSubmitAddress(uint64_t slotLeaderId) const {
  auto it = config_.mMiners.find(slotLeaderId);
  return it != config_.mMiners.end() ? it->second.endpoint : std::string{};
}

void MinerServer::requestMinerListRefresh() {
  const auto elapsed = std::chrono::steady_clock::now() - lastMinerListFetchTime_;
  if (elapsed >= MINER_LIST_REFETCH_INTERVAL) {
    minerListRefreshRequested_ = true;
  }
}

Client::Roe<void> MinerServer::dialLeader(uint64_t slotLeaderId, const std::string &multiaddr) {
  return forwardClient_.setAmpPeer("leader:" + std::to_string(slotLeaderId), multiaddr);
}

std::string MinerServer::handleParsedRequest(const Client::Request &request) {
  log().debug << "Handling request: " << request.type;
  auto it = requestHandlers_.find(request.type);
  Roe<std::string> result = (it != requestHandlers_.end())
                                ? it->second(request)
                                : hUnsupported(request);
  if (!result) {
    return Server::packResponse(1, result.error().message);
  }
  return Server::packResponse(result.value());
}

void MinerServer::dBlockGet(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid block get payload: " + request.payload)));
    return;
  }
  const uint64_t blockId = idResult.value();
  auto result = miner_.readBlock(blockId);
  if (result) {
    reply(packResponse(result.value().ltsToString()));
    return;
  }
  if (blockId < miner_.getNextBlockId()) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    return;
  }
  // Beyond our tip: answer after the run loop's next sync instead of syncing here.
  pendingBlockGets_.push_back({blockId, reply});
  blockSyncRequested_ = true;
}

void MinerServer::resolvePendingBlockGets() {
  if (pendingBlockGets_.empty()) {
    return;
  }
  for (auto &pending : pendingBlockGets_) {
    auto result = miner_.readBlock(pending.blockId);
    if (result) {
      pending.reply(packResponse(result.value().ltsToString()));
    } else {
      replyWith(pending.reply,
                Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    }
  }
  pendingBlockGets_.clear();
}

MinerServer::Roe<std::string>
MinerServer::hBlockAdd(const Client::Request &request) {
  Ledger::ChainNode block;
  if (!block.ltsFromString(request.payload)) {
    return Error(E_REQUEST, "Failed to deserialize block: " + request.payload);
  }
  block.hash = miner_.calculateHash(block.block);
  auto result = miner_.addBlock(block);
  if (!result) {
    return Error(E_REQUEST, "Failed to add block: " + result.error().message);
  }
  return {"Block added"};
}

MinerServer::Roe<std::string>
MinerServer::hAccountGet(const Client::Request &request) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    return Error(E_REQUEST, "Invalid account get payload: " + request.payload);
  }

  uint64_t accountId = idResult.value();
  auto result = miner_.getAccount(accountId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get account: " + result.error().message);
  }
  return result.value().ltsToString();
}

MinerServer::Roe<std::string>
MinerServer::hTxGetByWallet(const Client::Request &request) {
  auto reqResult = utl::binaryUnpack<Client::TxGetByWalletRequest>(request.payload);
  if (!reqResult) {
    return Error(E_REQUEST, "Failed to deserialize request: " + reqResult.error().message);
  }
  auto &req = reqResult.value();
  auto result = miner_.findTransactionsByWalletId(req.walletId, req.beforeBlockId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get transactions: " + result.error().message);
  }
  Client::TxGetByWalletResponse response;
  response.transactions = result.value();
  response.nextBlockId = req.beforeBlockId;
  return utl::binaryPack(response);
}

MinerServer::Roe<std::string>
MinerServer::hTxGetByIndex(const Client::Request &request) {
  auto reqResult = utl::binaryUnpack<Client::TxGetByIndexRequest>(request.payload);
  if (!reqResult) {
    return Error(E_REQUEST, "Failed to deserialize request: " + reqResult.error().message);
  }
  auto &req = reqResult.value();
  auto result = miner_.findTransactionByIndex(req.txIndex);
  if (!result) {
    return Error(E_REQUEST, "Failed to get transaction: " + result.error().message);
  }
  return utl::binaryPack(result.value());
}

void MinerServer::dTxAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  if (!miner_.isConfigReady()) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Miner syncing, please retry later")));
    return;
  }
  auto recResult = utl::binaryUnpack<Ledger::Record>(request.payload);
  if (!recResult) {
    replyWith(reply, Roe<std::string>(
                         Error(E_REQUEST, "Failed to deserialize transaction: " + recResult.error().message)));
    return;
  }

  const auto &record = recResult.value();
  if (miner_.isSlotLeader()) {
    auto result = miner_.addTransaction(record);
    replyWith(reply, result ? Roe<std::string>("Transaction added to pool")
                            : Roe<std::string>(Error(E_REQUEST, result.error().message)));
    return;
  }

  forwardToSlotLeader(record, miner_.getCurrentSlot(),
                      [reply](Roe<std::string> result) { replyWith(reply, result); });
}

void MinerServer::forwardToSlotLeader(const Ledger::Record &record, uint64_t slot,
                                      std::function<void(Roe<std::string>)> done) {
  auto cacheForRetry = [this, &record](const std::string &why) -> Roe<std::string> {
    if (!miner_.addToForwardCache(record)) {
      return Error(E_REQUEST, "Forward cache full, please retry later");
    }
    log().info << "Transaction cached for retry in next slot: " << why;
    return {"Transaction cached for retry in next slot"};
  };
  auto leaderIdResult = miner_.getSlotLeaderIdForSlot(slot);
  if (!leaderIdResult) {
    done(cacheForRetry("slot leader unknown: " + leaderIdResult.error().message));
    return;
  }
  const uint64_t leaderId = leaderIdResult.value();
  const std::string leaderAddr = lookupTxSubmitAddress(leaderId);
  if (leaderAddr.empty()) {
    requestMinerListRefresh();
    done(cacheForRetry("slot leader " + std::to_string(leaderId) + " address unknown"));
    return;
  }
  if (auto dial = dialLeader(leaderId, leaderAddr); !dial) {
    done(cacheForRetry("failed to dial slot leader: " + dial.error().message));
    return;
  }
  Client::TxForwardRequest forward;
  forward.record = record;
  forward.targetSlot = slot;
  forward.senderTipEpoch = miner_.getTipEpoch();
  forwardClient_.forwardTransactionAsync(
      forward, completeOnServerThread<std::string>(
                   [this, record, done = std::move(done)](Client::Roe<std::string> result) {
                     if (result) {
                       done(Roe<std::string>("Forwarded to slot leader: " + result.value()));
                     } else if (Client::isTransportError(result.error().code)) {
                       done(miner_.addToForwardCache(record)
                                ? Roe<std::string>("Transaction cached for retry in next slot")
                                : Roe<std::string>(Error(E_REQUEST, "Forward cache full, please retry later")));
                     } else {
                       done(Roe<std::string>(Error(E_REQUEST, result.error().message)));
                     }
                   }));
}

MinerServer::Roe<std::string>
MinerServer::hTxForward(const Client::Request &request) {
  if (!miner_.isConfigReady()) {
    return Error(E_REQUEST, "Miner syncing, please retry later");
  }
  auto forwardResult = utl::binaryUnpack<Client::TxForwardRequest>(request.payload);
  if (!forwardResult) {
    return Error(E_REQUEST, "Failed to deserialize forwarded transaction: " + forwardResult.error().message);
  }
  const auto &forward = forwardResult.value();
  auto cacheForward = [this](const Ledger::Record &record, std::string message) -> Roe<std::string> {
    if (!miner_.addToForwardCache(record)) {
      return Error(E_REQUEST, "Forward cache full, please retry later");
    }
    return message;
  };
  const uint64_t currentSlot = miner_.getCurrentSlot();
  switch (decideTxForward(miner_.getTipEpoch(), forward.senderTipEpoch, currentSlot, forward.targetSlot,
                          miner_.isSlotLeaderForSlot(forward.targetSlot))) {
  case TxForwardAction::AddToPool: {
    auto added = miner_.addTransaction(forward.record);
    if (!added) {
      return Error(E_REQUEST, added.error().message);
    }
    return {"Transaction added to pool"};
  }
  case TxForwardAction::HoldForSlot:
    // The slot-leader duty drains the cache into our pool when our slot starts.
    return cacheForward(forward.record, "Held for slot " + std::to_string(forward.targetSlot));
  case TxForwardAction::CacheBehind:
    blockSyncRequested_ = true;
    return cacheForward(forward.record, "Cached: receiver chain behind sender, retrying after sync");
  case TxForwardAction::CacheNotLeader:
    break;
  }
  return cacheForward(forward.record,
                      "Cached: not leader of slot " + std::to_string(forward.targetSlot) + ", retrying in next slot");
}

MinerServer::Roe<std::string>
MinerServer::hStatus(const Client::Request & /*request*/) {
  Client::MinerStatus status;

  status.minerId = config_.minerId;
  status.nextBlockId = miner_.getNextBlockId();
  status.pendingTransactions = miner_.getPendingTransactionCount();
  if (!miner_.isConfigReady()) {
    status.stake = 0;
    status.currentSlot = 0;
    status.currentEpoch = 0;
    status.nStakeholders = 0;
    status.isSlotLeader = false;
  } else {
    status.stake = miner_.getStake();
    status.currentSlot = miner_.getCurrentSlot();
    status.currentEpoch = miner_.getCurrentEpoch();
    status.nStakeholders = miner_.getStakeholders().size();
    status.isSlotLeader = miner_.isSlotLeader();
  }

  return utl::binaryPack(status.ltsToMeta());
}

MinerServer::Roe<std::string>
MinerServer::hCalibration(const Client::Request & /*request*/) {
  if (!miner_.isConfigReady()) {
    return Error(E_REQUEST, "Miner syncing, please retry later");
  }
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  Client::CalibrationResponse response;
  response.msTimestamp = nowMs + timeOffsetToBeaconMs_;
  response.nextBlockId = miner_.getNextBlockId();
  return utl::binaryPack(response);
}

MinerServer::Roe<std::string>
MinerServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST,
               "Unsupported request type: " + std::to_string(request.type));
}

void MinerServer::handleSlotLeaderRole() {
  // Add cached transactions to our own pool (we are slot leader, no need to
  // forward). addTransaction checks against this slot's fresh state, so a
  // rejection is final — re-caching would retry an invalid transaction forever.
  auto cached = miner_.drainForwardCache();
  size_t added = 0;
  for (const auto &signedTx : cached) {
    auto result = miner_.addTransaction(signedTx);
    if (result) {
      added++;
    } else {
      log().warning << "Dropping cached transaction rejected by slot leader pool: "
                    << result.error().message;
    }
  }
  if (added > 0) {
    log().info << "Added " << added << " cached transactions to slot leader pool";
  }

  static Ledger::ChainNode block;
  auto produceResult = miner_.produceBlock(block);
  if (!produceResult) {
    log().warning << "Failed to produce block: " +
                         produceResult.error().message;
    return;
  }

  if (!produceResult.value()) {
    // No block production needed
    return;
  }

  log().info << "Successfully produced block " << block.block.index
             << " with hash " << block.hash;

  // Broadcast for verification
  auto broadcastResult = broadcastBlock(block);
  if (!broadcastResult) {
    log().warning << "Failed to broadcast block: " +
                         broadcastResult.error().message;
    // Release the uncommitted seal, or every later seal/addBlock is refused.
    miner_.abandonBlock(block);
    // Most often another leader's block won this height: take it now rather
    // than re-sealing the same stale height until the next scheduled sync.
    trySyncBlocksFromBeacon(true);
    return;
  }

  log().info << "Block " << block.block.index << " broadcasted";
  miner_.markBlockProduction(block);

  auto addResult = miner_.addBlock(block);
  if (!addResult) {
    log().warning << "Failed to add block: " + addResult.error().message;
    return;
  }

  log().info << "Block produced successfully";
  log().info << "  Block ID: " << block.block.index;
  log().info << "  Slot: " << block.block.slot;
  log().info << "  Transactions: " << block.block.records.size();
  log().info << "  Hash: " << block.hash;
}

void MinerServer::retryCachedTransactionForwards() {
  // Only forward cached txes when in validator role; slot leader adds them itself
  if (miner_.isSlotLeader()) {
    return;
  }
  uint64_t currentSlot = miner_.getCurrentSlot();
  if (currentSlot == lastForwardRetrySlot_) {
    return;
  }
  auto cached = miner_.drainForwardCache();
  if (cached.empty()) {
    lastForwardRetrySlot_ = currentSlot;
    return;
  }
  lastForwardRetrySlot_ = currentSlot;
  // Transport failures go back into the cache (inside forwardToSlotLeader);
  // a leader's rejection is final, so an invalid transaction is not retried.
  for (const auto &tx : cached) {
    forwardToSlotLeader(tx, currentSlot, [this](Roe<std::string> result) {
      if (!result) {
        log().warning << "Dropping cached transaction rejected by slot leader: " << result.error().message;
      }
    });
  }
}

void MinerServer::handleValidatorRole() {
  retryCachedTransactionForwards();
  // Not slot leader - act as validator
  // Monitor for new blocks from other miners and validate them

  // In a full implementation, we would:
  // 1. Listen for blocks from the current slot leader
  // 2. Validate received blocks
  // 3. Add valid blocks to our chain
  // 4. Participate in consensus voting if required

  // For now, this is a placeholder for validator behavior
  // The actual block reception would happen via network requests
}

MinerServer::Roe<Client::BeaconState> MinerServer::connectToBeacon() {
  if (config_.network.beacon_multiaddrs.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
  }

  auto best = selectBestUpstreamIndex();
  if (!best) {
    return Error(E_NETWORK, best.error().message);
  }
  active_upstream_index_ = best.value();

  const auto& upstream_ma = config_.network.beacon_multiaddrs[active_upstream_index_];
  log().info << "Connecting to upstream: " << upstream_ma;

  if (auto dial = dialActiveUpstream(); !dial) {
    return Error(E_NETWORK, dial.error().message);
  }

  Client::MinerInfo minerInfo;
  minerInfo.id = config_.minerId;
  minerInfo.endpoint = listenMultiaddr();
  auto stateResult = client_.registerMinerServer(minerInfo);
  if (!stateResult) {
    return Error(E_NETWORK,
                 "Failed to register with upstream: " + stateResult.error().message);
  }

  const auto &state = stateResult.value();
  if (auto verified = verifyUpstreamState(state); !verified) {
    return Error(E_NETWORK, verified.error().message);
  }
  if (auto genesis = verifyGenesisAnchor(); !genesis) {
    return Error(E_NETWORK, genesis.error().message);
  }

  log().info << "Latest checkpoint ID: " << state.checkpointId;
  log().info << "Next block ID: " << state.nextBlockId;
  if (!state.networkId.empty()) {
    log().info << "Network ID: " << state.networkId;
  }

  return state;
}

MinerServer::Roe<void>
MinerServer::broadcastBlock(const Ledger::ChainNode &block) {
  bool anySuccess = false;
  for (const auto &beacon_ma : config_.network.beacon_multiaddrs) {
    if (auto dial = dialPeerMultiaddr(beacon_ma, "beacon"); !dial) {
      log().warning << "Failed to dial beacon " << beacon_ma << ": " << dial.error().message;
      continue;
    }
    auto clientResult = client_.addBlock(block);
    if (!clientResult) {
      log().warning << "Failed to add block to beacon " << beacon_ma << ": "
                    << clientResult.error().message;
      continue;
    }
    anySuccess = true;
  }
  if (!anySuccess) {
    return Error(E_NETWORK, "Failed to broadcast block to any beacon");
  }
  return {};
}

} // namespace pp
