#include "MinerServer.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace pp {

MinerServer::MinerServer() {
  redirectLogger("MinerServer");
  miner_.redirectLogger(log().getFullName() + ".Miner");
  client_.redirectLogger(log().getFullName() + ".Client");
}

// ============ RunFileConfig methods ============

nlohmann::json MinerServer::RunFileConfig::ltsToJson() {
  nlohmann::json j;
  j["minerId"] = minerId;
  j["keys"] = keys;
  j["host"] = host;
  j["port"] = port;
  j["beacons"] = beacons;
  return j;
}

MinerServer::Roe<void>
MinerServer::RunFileConfig::ltsFromJson(const nlohmann::json &jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
    }

    // Load and validate minerId (required)
    if (!jd.contains("minerId")) {
      return Error(E_CONFIG, "Field 'minerId' is required");
    }
    if (!jd["minerId"].is_number_unsigned()) {
      return Error(E_CONFIG, "Field 'minerId' must be a non-negative number");
    }
    minerId = jd["minerId"].get<uint64_t>();

    // Load and validate key files (required, supports "keys" array or legacy "key" string)
    if (jd.contains("keys")) {
      if (!jd["keys"].is_array()) {
        return Error(E_CONFIG, "Field 'keys' must be an array");
      }
      keys.clear();
      for (size_t i = 0; i < jd["keys"].size(); ++i) {
        const auto &k = jd["keys"][i];
        if (!k.is_string()) {
          return Error(E_CONFIG,
                       "All elements in 'keys' array must be strings (index " +
                           std::to_string(i) + " is not)");
        }
        std::string keyFile = k.get<std::string>();
        if (keyFile.empty()) {
          return Error(E_CONFIG,
                       "Key file at index " + std::to_string(i) + " cannot be empty");
        }
        keys.push_back(keyFile);
      }
      if (keys.empty()) {
        return Error(E_CONFIG, "Field 'keys' array must contain at least one key file");
      }
    } else {
      return Error(E_CONFIG, "Field 'keys' is required");
    }

    // Load and validate host
    if (jd.contains("host")) {
      if (!jd["host"].is_string()) {
        return Error(E_CONFIG, "Field 'host' must be a string");
      }
      host = jd["host"].get<std::string>();
      if (host.empty()) {
        return Error(E_CONFIG, "Field 'host' cannot be empty");
      }
    } else {
      host = Client::DEFAULT_HOST;
    }

    // Load and validate port
    if (jd.contains("port")) {
      if (!jd["port"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'port' must be a positive number");
      }
      uint64_t portValue = jd["port"].get<uint64_t>();
      if (portValue == 0 || portValue > 65535) {
        return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
      }
      port = static_cast<uint16_t>(portValue);
    } else {
      port = Client::DEFAULT_MINER_PORT;
    }

    // Load and validate beacons array (required, must have at least one)
    if (!jd.contains("beacons")) {
      return Error(E_CONFIG, "Field 'beacons' is required");
    }
    if (!jd["beacons"].is_array()) {
      return Error(E_CONFIG, "Field 'beacons' must be an array");
    }
    if (jd["beacons"].empty()) {
      return Error(
          E_CONFIG,
          "Field 'beacons' array must contain at least one beacon address");
    }

    beacons.clear();
    for (size_t i = 0; i < jd["beacons"].size(); ++i) {
      const auto &beacon = jd["beacons"][i];
      if (!beacon.is_string()) {
        return Error(E_CONFIG,
                     "All elements in 'beacons' array must be strings (index " +
                         std::to_string(i) + " is not)");
      }
      std::string beaconAddr = beacon.get<std::string>();
      if (beaconAddr.empty()) {
        return Error(E_CONFIG, "Beacon address at index " + std::to_string(i) +
                                   " cannot be empty");
      }
      beacons.push_back(beaconAddr);
    }

    if (beacons.empty()) {
      return Error(E_CONFIG, "Field 'beacons' array must contain at least one "
                             "valid string address");
    }

    return {};
  } catch (const std::exception &e) {
    return Error(E_CONFIG,
                 "Failed to parse run configuration: " + std::string(e.what()));
  }
}

// ============ MinerServer methods ============

MinerServer::~MinerServer() {}

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
    nlohmann::json defaultConfig = runFileConfig.ltsToJson();

    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_MINER,
                            "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << defaultConfig.dump(2) << std::endl;
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

    nlohmann::json config = jsonResult.value();
    auto parseResult = runFileConfig.ltsFromJson(config);
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
  config_.network.endpoint.address = runFileConfig.host;
  config_.network.endpoint.port = runFileConfig.port;
  config_.network.beacons = runFileConfig.beacons;

  log().info << "Configuration loaded";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Beacons: " << config_.network.beacons.size();

  auto serverStarted = startFetchServer(config_.network.endpoint);
  if (!serverStarted) {
    return Service::Error(E_MINER, "Failed to start FetchServer: " +
                                       serverStarted.error().message);
  }

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
  minerConfig.startingBlockId = state.lastCheckpointId;
  minerConfig.checkpointId = state.checkpointId;

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
  if (config_.network.beacons.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
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
  if (config_.network.beacons.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
  }

  std::string beaconAddr = config_.network.beacons[0];
  log().info << "Syncing blocks from beacon: " << beaconAddr;

  if (!client_.setEndpoint(beaconAddr)) {
    return Error(E_CONFIG, "Failed to resolve beacon address: " + beaconAddr);
  }

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

  auto &hgb = requestHandlers_[Client::T_REQ_BLOCK_GET];
  hgb = [this](const Client::Request &request) { return hBlockGet(request); };

  auto &hga = requestHandlers_[Client::T_REQ_ACCOUNT_GET];
  hga = [this](const Client::Request &request) { return hAccountGet(request); };

  auto &htx = requestHandlers_[Client::T_REQ_TX_GET_BY_WALLET];
  htx = [this](const Client::Request &request) { return hTxGetByWallet(request); };

  auto &hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab = [this](const Client::Request &request) { return hBlockAdd(request); };

  auto &hta = requestHandlers_[Client::T_REQ_TX_ADD];
  hta = [this](const Client::Request &request) { return hTxAdd(request); };
}

void MinerServer::onStop() {
  Server::onStop();
  log().info << "MinerServer resources cleaned up";
}

void MinerServer::runLoop() {
  log().info << "Block production and request handler loop started";

  while (!isStopSet()) {
    try {
      // Update miner state
      miner_.refresh();

      pollAndProcessOneRequest();

      syncBlocksPeriodically();

      if (miner_.isSlotLeader()) {
        handleSlotLeaderRole();
      } else {
        handleValidatorRole();
      }

      // Sleep for a short time before checking again
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    } catch (const std::exception &e) {
      log().error << "Exception in block production loop: " << e.what();
      std::this_thread::sleep_for(std::chrono::seconds(1));
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
  if (config_.network.beacons.empty()) {
    return;
  }
  std::string beaconAddr = config_.network.beacons[0];
  if (!client_.setEndpoint(beaconAddr)) {
    log().warning << "Failed to resolve beacon for miner list: " << beaconAddr;
    return;
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

std::string MinerServer::findTxSubmitAddress(uint64_t slotLeaderId) {
  auto it = config_.mMiners.find(slotLeaderId);
  if (it != config_.mMiners.end()) {
    return it->second.endpoint;
  }
  // Not found: refetch from beacon if enough time has elapsed
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      now - lastMinerListFetchTime_);
  if (elapsed >= MINER_LIST_REFETCH_INTERVAL) {
    log().info << "Slot leader " << slotLeaderId
               << " not in miner list, refetching from beacon";
    refreshMinerListFromBeacon();
    it = config_.mMiners.find(slotLeaderId);
    if (it != config_.mMiners.end()) {
      return it->second.endpoint;
    }
  }
  return "";
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

MinerServer::Roe<std::string>
MinerServer::hBlockGet(const Client::Request &request) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    return Error(E_REQUEST, "Invalid block get payload: " + request.payload);
  }
  uint64_t blockId = idResult.value();
  auto result = miner_.readBlock(blockId);
  if (!result) {
    // User requested block we don't have: sync from beacon then retry
    if (blockId >= miner_.getNextBlockId()) {
      trySyncBlocksFromBeacon(true);
      result = miner_.readBlock(blockId);
    }
    if (!result) {
      return Error(E_REQUEST, "Failed to get block: " + result.error().message);
    }
  }
  return result.value().ltsToString();
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
MinerServer::hTxAdd(const Client::Request &request) {
  auto signedTxResult =
      utl::binaryUnpack<Ledger::SignedData<Ledger::Transaction>>(
          request.payload);
  if (!signedTxResult) {
    return Error(E_REQUEST, "Failed to deserialize transaction: " +
                                signedTxResult.error().message);
  }

  const auto &signedTx = signedTxResult.value();
  if (miner_.isSlotLeader()) {
    auto result = miner_.addTransaction(signedTx);
    if (!result) {
      return Error(E_REQUEST, result.error().message);
    }
    return {"Transaction added to pool"};
  }

  auto slotLeaderIdResult = miner_.getSlotLeaderId();
  if (!slotLeaderIdResult) {
    return Error(E_REQUEST, slotLeaderIdResult.error().message);
  }
  uint64_t slotLeaderId = slotLeaderIdResult.value();
  std::string leaderAddr = findTxSubmitAddress(slotLeaderId);
  if (leaderAddr.empty()) {
    miner_.addToForwardCache(signedTx);
    log().info << "Slot leader " << slotLeaderId
               << " address unknown, transaction cached for retry in next slot";
    return {"Transaction cached for retry in next slot"};
  }
  if (!client_.setEndpoint(leaderAddr)) {
    return Error(E_CONFIG, "Failed to resolve leader address: " + leaderAddr);
  }

  auto result = client_.addTransaction(signedTx);
  if (!result) {
    return Error(E_REQUEST, result.error().message);
  }
  return {"Transaction submitted to slot leader"};
}

MinerServer::Roe<std::string>
MinerServer::hStatus(const Client::Request &request) {
  Client::MinerStatus status;

  status.minerId = config_.minerId;
  status.stake = miner_.getStake();
  status.nextBlockId = miner_.getNextBlockId();
  status.currentSlot = miner_.getCurrentSlot();
  status.currentEpoch = miner_.getCurrentEpoch();
  status.pendingTransactions = miner_.getPendingTransactionCount();
  status.nStakeholders = miner_.getStakeholders().size();
  status.isSlotLeader = miner_.isSlotLeader();

  return status.ltsToJson().dump();
}

MinerServer::Roe<std::string>
MinerServer::hCalibration(const Client::Request &request) {
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
  // Add cached transactions to our own pool (we are slot leader, no need to forward)
  auto cached = miner_.drainForwardCache();
  size_t added = 0;
  for (const auto &signedTx : cached) {
    auto result = miner_.addTransaction(signedTx);
    if (result) {
      added++;
    } else {
      miner_.addToForwardCache(signedTx);
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
  log().info << "  Transactions: " << block.block.signedTxes.size();
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
  auto slotLeaderIdResult = miner_.getSlotLeaderId();
  if (!slotLeaderIdResult) {
    for (const auto &tx : cached) {
      miner_.addToForwardCache(tx);
    }
    return;
  }
  uint64_t slotLeaderId = slotLeaderIdResult.value();
  std::string leaderAddr = findTxSubmitAddress(slotLeaderId);
  if (leaderAddr.empty()) {
    for (const auto &tx : cached) {
      miner_.addToForwardCache(tx);
    }
    log().debug << "Still cannot find slot leader " << slotLeaderId
                << " address, " << cached.size()
                << " transactions remain cached";
    return;
  }
  if (!client_.setEndpoint(leaderAddr)) {
    for (const auto &tx : cached) {
      miner_.addToForwardCache(tx);
    }
    return;
  }
  size_t forwarded = 0;
  for (const auto &signedTx : cached) {
    auto result = client_.addTransaction(signedTx);
    if (result) {
      forwarded++;
    } else {
      miner_.addToForwardCache(signedTx);
    }
  }
  if (forwarded > 0) {
    log().info << "Forwarded " << forwarded << " cached transactions to slot "
               << currentSlot << " leader";
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
  if (config_.network.beacons.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
  }

  // Try to connect to the first beacon in the list
  std::string beaconAddr = config_.network.beacons[0];
  log().info << "Connecting to beacon server: " << beaconAddr;

  if (!client_.setEndpoint(beaconAddr)) {
    return Error(E_CONFIG, "Failed to resolve beacon address: " + beaconAddr);
  }

  Client::MinerInfo minerInfo;
  minerInfo.id = config_.minerId;
  minerInfo.endpoint = getFetchServerEndpoint().ltsToString();
  auto stateResult = client_.registerMinerServer(minerInfo);
  if (!stateResult) {
    return Error(E_NETWORK,
                 "Failed to get beacon state: " + stateResult.error().message);
  }

  const auto &state = stateResult.value();
  log().info << "Latest checkpoint ID: " << state.checkpointId;
  log().info << "Next block ID: " << state.nextBlockId;

  return state;
}

MinerServer::Roe<void>
MinerServer::broadcastBlock(const Ledger::ChainNode &block) {
  bool anySuccess = false;
  for (const auto &beacon : config_.network.beacons) {
    if (!client_.setEndpoint(beacon)) {
      log().warning << "Failed to resolve beacon address: " + beacon;
      continue;
    }
    auto clientResult = client_.addBlock(block);
    if (!clientResult) {
      log().warning << "Failed to add block to beacon: " + beacon + ": " +
                           clientResult.error().message;
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
