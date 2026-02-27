#include "RelayServer.h"
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

// ============ BeaconConfig methods ============

nlohmann::json RelayServer::BeaconConfig::ltsToJson() {
  nlohmann::json j;
  j["host"] = host;
  j["port"] = port;
  j["dhtPort"] = dhtPort;
  return j;
}

RelayServer::Roe<void> RelayServer::BeaconConfig::ltsFromJson(const nlohmann::json &jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
    }
    // Load and validate host
    if (!jd.contains("host")) {
      return Error(E_CONFIG, "Field 'host' is required");
    }
    if (!jd["host"].is_string()) {
      return Error(E_CONFIG, "Field 'host' must be a string");
    }
    host = jd["host"].get<std::string>();
    if (host.empty()) {
      return Error(E_CONFIG, "Field 'host' cannot be empty");
    }
    // Load and validate port
    if (!jd.contains("port")) {
      return Error(E_CONFIG, "Field 'port' is required");
    }
    if (!jd["port"].is_number_unsigned()) {
      return Error(E_CONFIG, "Field 'port' must be a positive number");
    }
    uint64_t portValue = jd["port"].get<uint64_t>();
    if (portValue == 0 || portValue > 65535) {
      return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
    }
    port = static_cast<uint16_t>(portValue);
    // Load and validate dhtPort
    if (!jd.contains("dhtPort")) {
      return Error(E_CONFIG, "Field 'dhtPort' is required");
    }
    if (!jd["dhtPort"].is_number_unsigned()) {
      return Error(E_CONFIG, "Field 'dhtPort' must be a non-negative number");
    }
    uint64_t dhtPortValue = jd["dhtPort"].get<uint64_t>();
    if (dhtPortValue > 65535) {
      return Error(E_CONFIG, "Field 'dhtPort' must be between 0 and 65535");
    }
    dhtPort = static_cast<uint16_t>(dhtPortValue);
    return {};
  }
  catch (const std::exception &e) {
    return Error(E_CONFIG, "Failed to parse beacon configuration: " + std::string(e.what()));
  }
}

// ============ RunFileConfig methods ============

nlohmann::json RelayServer::RunFileConfig::ltsToJson() {
  nlohmann::json j;
  j["host"] = host;
  j["port"] = port;
  j["dhtPort"] = dhtPort;
  j["beacon"] = beacon.ltsToJson();
  return j;
}

RelayServer::Roe<void>
RelayServer::RunFileConfig::ltsFromJson(const nlohmann::json &jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
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
      port = Client::DEFAULT_BEACON_PORT;
    }

    // Load and validate beacon object {host, port, dhtPort}
    if (!jd.contains("beacon")) {
      return Error(E_CONFIG, "Field 'beacon' is required");
    }
    if (!jd["beacon"].is_object()) {
      return Error(E_CONFIG, "Field 'beacon' must be an object");
    }
    const auto &jb = jd["beacon"];
    if (!jb.contains("host") || !jb["host"].is_string()) {
      return Error(E_CONFIG, "Field 'beacon.host' is required and must be a string");
    }
    beacon.host = jb["host"].get<std::string>();
    if (beacon.host.empty()) {
      return Error(E_CONFIG, "Field 'beacon.host' cannot be empty");
    }
    if (!jb.contains("port") || !jb["port"].is_number_unsigned()) {
      return Error(E_CONFIG, "Field 'beacon.port' is required and must be a positive number");
    }
    uint64_t beaconPortValue = jb["port"].get<uint64_t>();
    if (beaconPortValue == 0 || beaconPortValue > 65535) {
      return Error(E_CONFIG, "Field 'beacon.port' must be between 1 and 65535");
    }
    beacon.port = static_cast<uint16_t>(beaconPortValue);
    if (!jb.contains("dhtPort") || !jb["dhtPort"].is_number_unsigned()) {
      return Error(E_CONFIG, "Field 'beacon.dhtPort' is required and must be a non-negative number");
    }
    uint64_t beaconDhtPortValue = jb["dhtPort"].get<uint64_t>();
    if (beaconDhtPortValue > 65535) {
      return Error(E_CONFIG, "Field 'beacon.dhtPort' must be between 0 and 65535");
    }
    beacon.dhtPort = static_cast<uint16_t>(beaconDhtPortValue);

    if (jd.contains("dhtPort")) {
      if (!jd["dhtPort"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'dhtPort' must be a non-negative number");
      }
      uint64_t v = jd["dhtPort"].get<uint64_t>();
      if (v > 65535) {
        return Error(E_CONFIG, "Field 'dhtPort' must be between 0 and 65535");
      }
      dhtPort = static_cast<uint16_t>(v);
    }

    return {};
  } catch (const std::exception &e) {
    return Error(E_CONFIG,
                 "Failed to parse run configuration: " + std::string(e.what()));
  }
}

// ============ RelayServer methods ============

RelayServer::RelayServer() {
  redirectLogger("RelayServer");
  relay_.redirectLogger(log().getFullName() + ".Relay");
  client_.redirectLogger(log().getFullName() + ".Client");
  dhtRunner_.redirectLogger(log().getFullName() + ".Dht");
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

    nlohmann::json defaultConfig = runFileConfig.ltsToJson();

    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_CONFIG,
                            "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << defaultConfig.dump(2) << std::endl;
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

    nlohmann::json config = jsonResult.value();
    auto parseResult = runFileConfig.ltsFromJson(config);
    if (!parseResult) {
      return Service::Error(E_CONFIG, "Failed to parse config file: " +
                                          parseResult.error().message);
    }
  }

  // Apply configuration from RunFileConfig
  config_.network.endpoint.address = runFileConfig.host;
  config_.network.endpoint.port = runFileConfig.port;
  config_.network.beacon.address = runFileConfig.beacon.host;
  config_.network.beacon.port = runFileConfig.beacon.port;
  config_.network.beaconDhtPort = runFileConfig.beacon.dhtPort;

  log().info << "Configuration loaded";
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Beacon: " << config_.network.beacon
             << " (DHT UDP " << config_.network.beaconDhtPort << ")";

  auto serverStarted = startFetchServer(config_.network.endpoint);
  if (!serverStarted) {
    return Service::Error(E_NETWORK, "Failed to start FetchServer: " +
                                         serverStarted.error().message);
  }

  // Start DHT (bootstrap from beacon's DHT endpoint)
  network::DhtRunner::Config dhtConfig;
  network::IpEndpoint beacon = {config_.network.beacon.address, config_.network.beaconDhtPort};
  dhtConfig.bootstrapEndpoints = {beacon.ltsToString()};
  dhtConfig.dhtPort = runFileConfig.dhtPort;
  dhtConfig.myTcpPort = config_.network.endpoint.port;
  dhtConfig.networkId = network::DhtRunner::getDefaultNetworkId();
  dhtConfig.nodeIdPath = getWorkDir() + "/dht-node.id";
  auto dhtStart = dhtRunner_.start(dhtConfig);
  if (!dhtStart) {
    return Service::Error(E_NETWORK, "Failed to start DHT: " + dhtStart.error().message);
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

  auto syncResult = syncBlocksFromBeacon();
  if (!syncResult) {
    return Service::Error(E_NETWORK, "Failed to sync blocks from beacon: " +
                                         syncResult.error().message);
  }
  lastBlockSyncTime_ = std::chrono::steady_clock::now();
  lastSyncedEpoch_ = relay_.getCurrentEpoch();

  log().info << "Relay core initialized";
  log().info << "  Next block ID: " << relay_.getNextBlockId();

  initHandlers();
  log().info << "RelayServer initialization complete";
  return {};
}

RelayServer::Roe<void> RelayServer::syncBlocksFromBeacon() {
  std::string beaconAddr =
      config_.network.beacon.address + ":" + std::to_string(config_.network.beacon.port);
  log().info << "Syncing blocks from beacon: " << beaconAddr;

  client_.setEndpoint(config_.network.beacon);

  auto calibrationResult = client_.fetchCalibration();
  if (!calibrationResult) {
    return Error(E_NETWORK,
                 "Failed to get beacon calibration: " + calibrationResult.error().message);
  }

  uint64_t latestBlockId = calibrationResult.value().nextBlockId;
  uint64_t nextBlockId = relay_.getNextBlockId();

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
    block.hash = relay_.calculateHash(block.block);

    auto addResult = relay_.addBlock(block);
    if (!addResult) {
      return Error(E_RELAY, "Failed to add block " + std::to_string(blockId) +
                                ": " + addResult.error().message);
    }

    log().debug << "Synced block " << blockId;
  }

  log().info << "Sync complete: " << (latestBlockId - nextBlockId)
             << " blocks added";
  return {};
}

void RelayServer::initHandlers() {
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

  auto &htxi = requestHandlers_[Client::T_REQ_TX_GET_BY_INDEX];
  htxi = [this](const Client::Request &request) { return hTxGetByIndex(request); };

  auto &hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab = [this](const Client::Request &request) { return hBlockAdd(request); };

  auto &hreg = requestHandlers_[Client::T_REQ_REGISTER];
  hreg = [this](const Client::Request &request) { return hRegister(request); };

  auto &hml = requestHandlers_[Client::T_REQ_MINER_LIST];
  hml = [this](const Client::Request &request) { return hMinerList(request); };
};

void RelayServer::onStop() {
  dhtRunner_.stop();
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
  state.lastCheckpointId = relay_.getLastCheckpointId();
  state.checkpointId = relay_.getCurrentCheckpointId();
  state.nextBlockId = relay_.getNextBlockId();
  state.currentSlot = relay_.getCurrentSlot();
  state.currentEpoch = relay_.getCurrentEpoch();
  state.nStakeholders = relay_.getStakeholders().size();

  return state;
}

void RelayServer::runLoop() {
  log().info << "Request handler loop started";

  while (!isStopSet()) {
    try {
      // Update relay state
      relay_.refresh();

      syncBlocksPeriodically();

      // Process queued requests
      if (!pollAndProcessOneRequest()) {
        // Sleep for a short time if queue is not too busy
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } catch (const std::exception &e) {
      log().error << "Exception in request handler loop: " << e.what();
      std::this_thread::sleep_for(std::chrono::seconds(1));
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
  auto syncResult = syncBlocksFromBeacon();
  if (syncResult) {
    lastBlockSyncTime_ = std::chrono::steady_clock::now();
    lastSyncedEpoch_ = relay_.getCurrentEpoch();
  } else {
    log().warning << "Block sync failed: " << syncResult.error().message;
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

RelayServer::Roe<std::string>
RelayServer::hBlockGet(const Client::Request &request) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    return Error(E_REQUEST, "Invalid block get payload: " + request.payload);
  }

  uint64_t blockId = idResult.value();
  auto result = relay_.readBlock(blockId);
  if (!result) {
    // User requested block we don't have: sync from beacon then retry
    if (blockId >= relay_.getNextBlockId()) {
      trySyncBlocksFromBeacon(true);
      result = relay_.readBlock(blockId);
    }
    if (!result) {
      return Error(E_REQUEST, "Failed to get block: " + result.error().message);
    }
  }

  return result.value().ltsToString();
}

RelayServer::Roe<std::string>
RelayServer::hBlockAdd(const Client::Request &request) {
  Ledger::ChainNode block;
  if (!block.ltsFromString(request.payload)) {
    return Error(E_REQUEST, "Failed to deserialize block: " + request.payload);
  }
  client_.setEndpoint(config_.network.beacon);
  auto result = client_.addBlock(block);
  if (!result) {
    return Error(E_NETWORK, result.error().message);
  }
  relay_.addBlock(block); // Don't care about the result
  return {"Block added"};
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

RelayServer::Roe<std::string>
RelayServer::hRegister(const Client::Request &request) {
  Client::MinerInfo minerInfo;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(request.payload);
  } catch (const std::exception &e) {
    return Error(E_REQUEST,
                 "Failed to parse miner info: " + std::string(e.what()));
  }
  auto parseResult = minerInfo.ltsFromJson(j);
  if (!parseResult) {
    return Error(E_REQUEST, parseResult.error().message);
  }
  registerServer(minerInfo);
  return buildStateResponse().ltsToJson().dump();
}

RelayServer::Roe<std::string>
RelayServer::hStatus(const Client::Request &request) {
  return buildStateResponse().ltsToJson().dump();
}

RelayServer::Roe<std::string>
RelayServer::hCalibration(const Client::Request &request) {
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  Client::CalibrationResponse response;
  response.msTimestamp = nowMs + timeOffsetToBeaconMs_;
  response.nextBlockId = relay_.getNextBlockId();
  return utl::binaryPack(response);
}

RelayServer::Roe<int64_t> RelayServer::calibrateTimeToBeacon() {
  client_.setEndpoint(config_.network.beacon);

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

RelayServer::Roe<std::string>
RelayServer::hMinerList(const Client::Request &request) {
  nlohmann::json j = nlohmann::json::array();
  for (const auto &[id, info] : mMiners_) {
    j.push_back(info.ltsToJson());
  }
  return j.dump();
}

RelayServer::Roe<std::string>
RelayServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST,
               "Unsupported request type: " + std::to_string(request.type));
}

} // namespace pp
