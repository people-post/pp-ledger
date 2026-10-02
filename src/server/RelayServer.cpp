#include "RelayServer.h"
#include "BlockAddPolicy.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "../network/amp/AmpIdentity.h"
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

// ============ RunFileConfig methods ============

Object RelayServer::RunFileConfig::ltsToJson() {
  Object j;
  j.setJsonUInt("port", port);
  std::vector<Value> keyVals;
  for (const auto &k : keys) {
    keyVals.push_back(k);
  }
  j.set("keys", Object::array(std::move(keyVals)));
  if (!beacon.empty()) {
    j.set("beacon", beacon);
  } else {
    Object beaconObj;
    beaconObj.set("host", Client::DEFAULT_HOST);
    beaconObj.setJsonUInt("port", Client::DEFAULT_BEACON_PORT);
    j.set("beacon", beaconObj);
  }
  return j;
}

RelayServer::Roe<void>
RelayServer::RunFileConfig::ltsFromJson(const Object &jd) {
  if (jd.contains("port")) {
    auto portValue = jd.getNonNegInt("port");
    if (!portValue || *portValue == 0 || *portValue > 65535) {
      return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
    }
    port = static_cast<uint16_t>(*portValue);
  } else {
    port = DEFAULT_RELAY_PORT;
  }

  if (jd.contains("keys")) {
    const Array *keysArr = jd.getArray("keys");
    if (!keysArr) {
      return Error(E_CONFIG, "Field 'keys' must be an array");
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
  } else {
    keys = {FILE_AMP_IDENTITY};
  }

  if (auto beaconStr = jd.getString("beacon")) {
    auto ma = network::ParseBeaconMultiaddrString(*beaconStr);
    if (!ma) {
      return Error(E_CONFIG, "Failed to parse beacon multiaddr: " + ma.error().message);
    }
    beacon = std::move(*ma);
  } else {
    const Object *beaconObj = jd.getObject("beacon");
    if (!beaconObj) {
      return Error(E_CONFIG, jd.contains("beacon")
                                 ? "Field 'beacon' must be a multiaddr string or object"
                                 : "Field 'beacon' is required");
    }
    auto ma = network::ParseBeaconMultiaddr(*beaconObj);
    if (!ma) {
      return Error(E_CONFIG, "Failed to parse beacon configuration: " + ma.error().message);
    }
    beacon = std::move(*ma);
  }

  auto tuningResult = network::NetworkTuning::fromConfig(jd);
  if (!tuningResult) {
    return Error(E_CONFIG, tuningResult.error().message);
  }
  tuning = std::move(*tuningResult);
  return {};
}

// ============ RelayServer methods ============

RelayServer::RelayServer() {
  redirectLogger("RelayServer");
  relay_.redirectLogger(log().getFullName() + ".Relay");
  client_.redirectLogger(log().getFullName() + ".Client");
}

Client::Roe<void> RelayServer::dialPeerMultiaddr(const std::string& multiaddr,
                                                 const std::string& peer_key) {
  return client_.setAmpPeer(peer_key, multiaddr);
}

Service::Roe<void> RelayServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(getWorkDir()) / FILE_CONFIG;
  std::string configPathStr = configPath.string();

  // Create default FILE_CONFIG if it doesn't exist using RunFileConfig
  RunFileConfig runFileConfig;

  if (!std::filesystem::exists(configPath)) {
    log().info << "No " << FILE_CONFIG
               << " found, creating with default values";

    auto encoded = encodeObjectPretty(runFileConfig.ltsToJson());
    if (!encoded) {
      return Service::Error(E_CONFIG, "Failed to encode " + std::string(FILE_CONFIG) +
                                          ": " + encoded.error().message);
    }
    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_CONFIG,
                            "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << encoded.value() << std::endl;
    configFile.close();

    log().info << "Created " << FILE_CONFIG << " at: " << configPathStr;
    log().info << "Please edit " << FILE_CONFIG
               << " to configure your relay settings";
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
  config_.network.udp_port = runFileConfig.port;
  config_.network.beacon_multiaddr = runFileConfig.beacon;
  config_.network.privateKeys.clear();
  for (const auto &keyFile : runFileConfig.keys) {
    auto keyResult = utl::readPrivateKey(keyFile, getWorkDir());
    if (!keyResult) {
      return Service::Error(E_CONFIG,
                            "Failed to load key '" + keyFile + "': " +
                                keyResult.error().message);
    }
    config_.network.privateKeys.push_back(keyResult.value());
  }

  log().info << "Configuration loaded";
  log().info << "  UDP port: " << config_.network.udp_port;
  log().info << "  Beacon: " << config_.network.beacon_multiaddr;

  setNetworkTuning(runFileConfig.tuning);
  std::vector<std::string> upstreamAddrs;
  if (!config_.network.beacon_multiaddr.empty()) {
    upstreamAddrs.push_back(config_.network.beacon_multiaddr);
  }
  if (auto upstreams = setUpstreams(upstreamAddrs); !upstreams) {
    return Service::Error(E_CONFIG, upstreams.error().message);
  }
  client_.setRequestTimeout(networkTuning().rpcTimeout);

  auto ampCfg = network::LedgerAmpConfigFromPrivateKey(config_.network.privateKeys.front(),
                                                       config_.network.udp_port);
  if (!ampCfg) {
    return Service::Error(E_CONFIG, "Failed to build AMP config: " + ampCfg.error().message);
  }
  auto serverStarted = startAmpServer(*ampCfg);
  if (!serverStarted) {
    return Service::Error(E_NETWORK, "Failed to start AMP server: " +
                                         serverStarted.error().message);
  }

  if (!ampRuntime()) {
    return Service::Error(E_NETWORK, "AMP runtime unavailable after start");
  }
  client_.attachAmpTransport(*ampRuntime(), "beacon");

  if (!config_.network.beacon_multiaddr.empty()) {
    if (auto dial = dialPeerMultiaddr(config_.network.beacon_multiaddr, "beacon"); !dial) {
      return Service::Error(E_NETWORK, "Failed to dial beacon: " + dial.error().message);
    }
  }

  // Initialize Relay with starting block id 0 (no beacon sync, no block
  // production)
  std::filesystem::path relayDataDir =
      std::filesystem::path(getWorkDir()) / DIR_DATA;
  Relay::InitConfig relayConfig;
  relayConfig.workDir = relayDataDir.string();
  relayConfig.timeOffset = 0;
  relayConfig.startingBlockId = 0;

  {
    auto offsetResult = calibrateTimeToBeacon();
    if (offsetResult) {
      timeOffsetToBeaconMs_ = offsetResult.value();
      relayConfig.timeOffset = timeOffsetToBeaconMs_ / 1000;
    } else {
      log().warning << "Time calibration skipped: " << offsetResult.error().message;
    }
  }

  auto relayInit = relay_.init(relayConfig);
  if (!relayInit) {
    return Service::Error(E_RELAY, "Failed to initialize Relay: " +
                                       relayInit.error().message);
  }

  // Initial catch-up through the same BlockSync as at run time; completions
  // are served here because the run loop has not started yet.
  initBlockSync();
  lastSyncResult_.reset();
  if (auto error = runStartupSync(*blockSync_, lastSyncResult_, networkTuning().startupSyncTimeout,
                                  [this]() { return !beaconStateRefreshInFlight_; });
      !error.empty()) {
    return Service::Error(E_NETWORK, "Failed to sync blocks from beacon: " + error);
  }

  log().info << "Relay core initialized";
  log().info << "  Next block ID: " << relay_.getNextBlockId();

  initHandlers();
  log().info << "RelayServer initialization complete";
  return {};
}

void RelayServer::initBlockSync() {
  BlockSync::Hooks hooks;
  hooks.dialUpstream = [this]() -> pp::Roe<void> {
    if (config_.network.beacon_multiaddr.empty()) {
      return pp::Error("No beacon server configured");
    }
    auto dial = dialPeerMultiaddr(config_.network.beacon_multiaddr, "beacon");
    return dial ? pp::Roe<void>() : pp::Roe<void>(pp::Error(dial.error().message));
  };
  hooks.nextBlockId = [this]() { return relay_.getNextBlockId(); };
  hooks.applyBlock = [this](const Ledger::ChainNode &fetched) -> pp::Roe<void> {
    Ledger::ChainNode block = fetched;
    block.hash = relay_.calculateHash(block.block);
    auto added = relay_.addBlock(block);
    return added ? pp::Roe<void>() : pp::Roe<void>(pp::Error(added.error().message));
  };
  hooks.postToServerThread = [this](std::function<void()> task) { postToServerThread(std::move(task)); };
  hooks.onFinished = [this](const BlockSync::Result &result) { onBlockSyncFinished(result); };
  UpstreamTipWatch::Hooks watchHooks;
  watchHooks.dialUpstream = hooks.dialUpstream;
  watchHooks.nextBlockId = hooks.nextBlockId;
  watchHooks.onUpstreamAhead = [this]() { blockSyncRequested_ = true; };
  watchHooks.postToServerThread = hooks.postToServerThread;
  blockSync_ = std::make_unique<BlockSync>(client_, std::move(hooks));
  blockSync_->redirectLogger(log().getFullName() + ".BlockSync");
  tipWatch_ = std::make_unique<UpstreamTipWatch>(client_, std::move(watchHooks));
  tipWatch_->redirectLogger(log().getFullName() + ".TipWatch");
}

void RelayServer::onBlockSyncFinished(const BlockSync::Result &result) {
  lastSyncResult_ = result;
  if (result.ok) {
    lastBlockSyncTime_ = std::chrono::steady_clock::now();
    lastSyncedEpoch_ = relay_.getCurrentEpoch();
    startBeaconStateRefresh();
  }
  resolvePendingBlockGets();
}

void RelayServer::startBeaconStateRefresh() {
  if (beaconStateRefreshInFlight_) {
    return;
  }
  beaconStateRefreshInFlight_ = true;
  client_.fetchBeaconStateAsync(completeOnServerThread<Client::BeaconState>(
      [this](Client::Roe<Client::BeaconState> status) {
        beaconStateRefreshInFlight_ = false;
        if (!status) {
          log().warning << "Failed to refresh beacon state: " << status.error().message;
          return;
        }
        registryVersion_ = status.value().registryVersion;
        if (!status.value().networkId.empty()) {
          networkId_ = status.value().networkId;
        }
      }));
}

void RelayServer::initHandlers() {
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


  // Handlers that need the beacon (or a sync) reply later; see handleDeferred.
  deferredHandlers_.clear();
  deferredHandlers_[Client::T_REQ_BLOCK_GET] = [this](const Client::Request &r, const RequestQueue::Reply &reply) {
    dBlockGet(r, reply);
  };
  deferredHandlers_[Client::T_REQ_BLOCK_ADD] = [this](const Client::Request &r, const RequestQueue::Reply &reply) {
    dBlockAdd(r, reply);
  };
  deferredHandlers_[Client::T_REQ_REGISTER] = [this](const Client::Request &r, const RequestQueue::Reply &reply) {
    dRegister(r, reply);
  };
  deferredHandlers_[Client::T_REQ_MINER_LIST] = [this](const Client::Request &r, const RequestQueue::Reply &reply) {
    dMinerList(r, reply);
  };
}

bool RelayServer::handleDeferred(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto it = deferredHandlers_.find(request.type);
  if (it == deferredHandlers_.end()) {
    return false;
  }
  it->second(request, reply);
  return true;
}

void RelayServer::onStop() {
  // Reply before AMP stops (Server::onStop), while replies can still be sent.
  for (auto &pending : pendingBlockGets_) {
    pending.reply(packResponse(Client::E_SERVER_ERROR, "Server stopping"));
  }
  pendingBlockGets_.clear();
  Server::onStop();
  log().info << "RelayServer resources cleaned up";
}

void RelayServer::registerServer(const Client::MinerInfo &minerInfo) {
  mMiners_[minerInfo.id] = minerInfo;
  log().debug << "Updated miner record: " << minerInfo.id << " "
              << minerInfo.endpoint;
}

Client::BeaconState RelayServer::buildStateResponse() const {
  int64_t currentTimestamp =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  Client::BeaconState state;
  state.currentTimestamp = currentTimestamp;
  const auto checkpoint = relay_.getCheckpoint();
  state.checkpointId = checkpoint.lastId;  // User lastId so that miners can replay blocks to current checkpoint
  state.nextBlockId = relay_.getNextBlockId();
  state.currentSlot = relay_.getCurrentSlot();
  state.currentEpoch = relay_.getCurrentEpoch();
  state.nStakeholders = relay_.getStakeholders().size();
  state.networkId = networkId_;
  state.registryVersion = registryVersion_;
  if (state.nextBlockId > 0) {
    if (auto tip = relay_.readBlock(state.nextBlockId - 1)) {
      state.headHash = tip.value().hash;
    }
  }

  return state;
}

void RelayServer::runLoop() {
  log().info << "Request handler loop started";

  while (!isStopSet()) {
    try {
      relay_.refresh();
      syncBlocksPeriodically();
      tipWatch_->maintain();
      if (blockSyncRequested_) {
        blockSyncRequested_ = false;
        trySyncBlocksFromBeacon(true);
      }
      serveRequestsFor(std::chrono::milliseconds(100));
    } catch (const std::exception& e) {
      log().error << "Exception in request handler loop: " << e.what();
      serveRequestsFor(std::chrono::seconds(1));
    }
  }

  log().info << "Request handler loop stopped";
}

void RelayServer::trySyncBlocksFromBeacon(bool bypassRateLimit) {
  const uint64_t slotDurationSec = relay_.getSlotDuration();
  if (!bypassRateLimit && slotDurationSec > 0) {
    auto now = std::chrono::steady_clock::now();
    auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastBlockSyncTime_).count();
    if (elapsedSec < static_cast<int64_t>(slotDurationSec)) {
      return; // Rate limit: at most one sync per slot time
    }
  }
  if (blockSync_->start() == BlockSync::Start::BackingOff) {
    // No sync coming soon: answer waiting block gets with what we have.
    resolvePendingBlockGets();
  }
}

void RelayServer::syncBlocksPeriodically() {
  const uint64_t currentEpoch = relay_.getCurrentEpoch();
  const uint64_t slotDurationSec = relay_.getSlotDuration();
  if (slotDurationSec == 0) {
    return;
  }

  // At beginning of each epoch: sync to update stakeholders (relay never produces blocks)
  const bool needSyncForEpoch = (currentEpoch > lastSyncedEpoch_);
  if (!needSyncForEpoch) {
    return;
  }

  trySyncBlocksFromBeacon(false);
}

std::string RelayServer::handleParsedRequest(const Client::Request &request) {
  auto it = requestHandlers_.find(request.type);
  Roe<std::string> result = (it != requestHandlers_.end())
                                ? it->second(request)
                                : hUnsupported(request);
  if (!result) {
    return Server::packResponse(1, result.error().message);
  }
  return Server::packResponse(result.value());
}

void RelayServer::dBlockGet(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid block get payload: " + request.payload)));
    return;
  }
  const uint64_t blockId = idResult.value();
  auto result = relay_.readBlock(blockId);
  if (result) {
    reply(packResponse(result.value().ltsToString()));
    return;
  }
  if (blockId < relay_.getNextBlockId()) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    return;
  }
  // Beyond our tip: answer after the run loop's next sync instead of syncing here.
  pendingBlockGets_.push_back({blockId, reply});
  blockSyncRequested_ = true;
}

void RelayServer::resolvePendingBlockGets() {
  if (pendingBlockGets_.empty()) {
    return;
  }
  for (auto &pending : pendingBlockGets_) {
    auto result = relay_.readBlock(pending.blockId);
    if (result) {
      pending.reply(packResponse(result.value().ltsToString()));
    } else {
      replyWith(pending.reply,
                Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    }
  }
  pendingBlockGets_.clear();
}

void RelayServer::dBlockAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  Ledger::ChainNode block;
  if (!block.ltsFromString(request.payload)) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to deserialize block: " + request.payload)));
    return;
  }
  switch (checkBlockAdd(relay_, block)) {
  case BlockAddCheck::AlreadyHave:
    reply(packResponse("Block already added")); // we synced it from upstream: it is there already
    return;
  case BlockAddCheck::Conflicts:
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Block " + std::to_string(block.block.index) +
                                                           " conflicts with the stored block")));
    return;
  case BlockAddCheck::New:
    break;
  }
  if (auto dial = dialPeerMultiaddr(config_.network.beacon_multiaddr, "beacon"); !dial) {
    replyWith(reply, Roe<std::string>(Error(E_NETWORK, "Failed to dial beacon: " + dial.error().message)));
    return;
  }
  client_.addBlockAsync(block, completeOnServerThread<bool>([this, block, reply](Client::Roe<bool> result) {
    if (!result) {
      replyWith(reply, Roe<std::string>(Error(E_NETWORK, result.error().message)));
      return;
    }
    relay_.addBlock(block); // Don't care about the result
    reply(packResponse("Block added"));
  }));
}

RelayServer::Roe<std::string>
RelayServer::hAccountGet(const Client::Request &request) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    return Error(E_REQUEST, "Invalid account get payload: " + request.payload);
  }

  uint64_t accountId = idResult.value();
  auto result = relay_.getAccount(accountId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get account: " + result.error().message);
  }
  return result.value().ltsToString();
}

RelayServer::Roe<std::string>
RelayServer::hTxGetByWallet(const Client::Request &request) {
  auto reqResult = utl::binaryUnpack<Client::TxGetByWalletRequest>(request.payload);
  if (!reqResult) {
    return Error(E_REQUEST, "Failed to deserialize request: " + reqResult.error().message);
  }
  auto &req = reqResult.value();
  auto result = relay_.findTransactionsByWalletId(req.walletId, req.beforeBlockId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get transactions: " + result.error().message);
  }
  Client::TxGetByWalletResponse response;
  response.transactions = result.value();
  response.nextBlockId = req.beforeBlockId;
  return utl::binaryPack(response);
}

RelayServer::Roe<std::string>
RelayServer::hTxGetByIndex(const Client::Request &request) {
  auto reqResult = utl::binaryUnpack<Client::TxGetByIndexRequest>(request.payload);
  if (!reqResult) {
    return Error(E_REQUEST, "Failed to deserialize request: " + reqResult.error().message);
  }
  auto &req = reqResult.value();
  auto result = relay_.findTransactionByIndex(req.txIndex);
  if (!result) {
    return Error(E_REQUEST, "Failed to get transaction: " + result.error().message);
  }
  return utl::binaryPack(result.value());
}

void RelayServer::dRegister(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto unpacked = utl::binaryUnpack<pp::common::Meta>(request.payload);
  if (!unpacked) {
    replyWith(reply, Roe<std::string>(
                         Error(E_REQUEST, "Failed to unpack miner info Meta: " + unpacked.error().message)));
    return;
  }
  Client::MinerInfo minerInfo;
  auto parsed = minerInfo.ltsFromMeta(unpacked.value());
  if (!parsed) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, parsed.error().message)));
    return;
  }
  if (config_.network.beacon_multiaddr.empty()) {
    replyWith(reply, Roe<std::string>(Error(E_CONFIG, "No upstream configured")));
    return;
  }
  if (auto dial = dialPeerMultiaddr(config_.network.beacon_multiaddr, "beacon"); !dial) {
    replyWith(reply, Roe<std::string>(Error(E_NETWORK, "Failed to dial upstream: " + dial.error().message)));
    return;
  }
  client_.registerMinerServerAsync(
      minerInfo, completeOnServerThread<Client::BeaconState>(
                     [this, minerInfo, reply](Client::Roe<Client::BeaconState> state) {
                       if (!state) {
                         replyWith(reply, Roe<std::string>(Error(
                                              E_NETWORK, "Upstream register failed: " + state.error().message)));
                         return;
                       }
                       registerServer(minerInfo);
                       registryVersion_ = state.value().registryVersion;
                       if (!state.value().networkId.empty()) {
                         networkId_ = state.value().networkId;
                       }
                       reply(packResponse(utl::binaryPack(state.value().ltsToMeta())));
                     }));
}

RelayServer::Roe<std::string>
RelayServer::hStatus(const Client::Request & /*request*/) {
  return utl::binaryPack(buildStateResponse().ltsToMeta());
}

RelayServer::Roe<std::string>
RelayServer::hCalibration(const Client::Request & /*request*/) {
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  Client::CalibrationResponse response;
  response.msTimestamp = nowMs + timeOffsetToBeaconMs_;
  response.nextBlockId = relay_.getNextBlockId();
  return utl::binaryPack(response);
}

RelayServer::Roe<int64_t> RelayServer::calibrateTimeToBeacon() {
  if (config_.network.beacon_multiaddr.empty()) {
    return Error(E_CONFIG, "No beacon server configured");
  }
  if (auto dial = dialPeerMultiaddr(config_.network.beacon_multiaddr, "beacon"); !dial) {
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
      log().info << "Time calibrated to beacon: offset=" << offsetMs << " ms, RTT=" << rttMs << " ms (single sample)";
      return offsetMs;
    }
    if (i == 0) {
      log().debug << "High RTT (" << rttMs << " ms), taking up to " << CALIBRATION_SAMPLES << " samples";
    }
  }

  auto best = std::min_element(samples.begin(), samples.end(),
                              [](const Sample &a, const Sample &b) { return a.rttMs < b.rttMs; });
  int64_t offsetMs = best->offsetMs;
  log().info << "Time calibrated to beacon: offset=" << offsetMs << " ms, samples=" << samples.size()
             << ", min RTT=" << best->rttMs << " ms";
  return offsetMs;
}

void RelayServer::dMinerList(const Client::Request & /*request*/, const RequestQueue::Reply &reply) {
  if (config_.network.beacon_multiaddr.empty()) {
    replyWith(reply, Roe<std::string>(Error(E_CONFIG, "No upstream configured")));
    return;
  }
  if (auto dial = dialPeerMultiaddr(config_.network.beacon_multiaddr, "beacon"); !dial) {
    replyWith(reply, Roe<std::string>(Error(E_NETWORK, "Failed to dial upstream: " + dial.error().message)));
    return;
  }
  client_.fetchMinerListAsync(completeOnServerThread<std::vector<Client::MinerInfo>>(
      [this, reply](Client::Roe<std::vector<Client::MinerInfo>> miners) {
        if (!miners) {
          replyWith(reply, Roe<std::string>(Error(
                               E_NETWORK, "Failed to fetch miner list from upstream: " + miners.error().message)));
          return;
        }
        mMiners_.clear();
        std::vector<pp::common::Meta> list;
        list.reserve(miners.value().size());
        for (const auto &miner : miners.value()) {
          mMiners_[miner.id] = miner;
          list.push_back(miner.ltsToMeta());
        }
        reply(packResponse(utl::binaryPack(list)));
      }));
}

RelayServer::Roe<std::string>
RelayServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST,
               "Unsupported request type: " + std::to_string(request.type));
}

} // namespace pp
