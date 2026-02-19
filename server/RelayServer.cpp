#include "RelayServer.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pp {

RelayServer::RelayServer() {
  redirectLogger("RelayServer");
  relay_.redirectLogger(log().getFullName() + ".Relay");
  client_.redirectLogger(log().getFullName() + ".Client");
}

// ============ RunFileConfig methods ============

nlohmann::json RelayServer::RunFileConfig::ltsToJson() {
  nlohmann::json j;
  j["host"] = host;
  j["port"] = port;
  j["beacon"] = beacon;
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

    // Load and validate beacon
    if (jd.contains("beacon")) {
      if (!jd["beacon"].is_string()) {
        return Error(E_CONFIG, "Field 'beacon' must be a string");
      }
      beacon = jd["beacon"].get<std::string>();
    }

    if (beacon.empty()) {
      return Error(E_CONFIG, "Field 'beacon' cannot be empty");
    }

    return {};
  } catch (const std::exception &e) {
    return Error(E_CONFIG,
                 "Failed to parse run configuration: " + std::string(e.what()));
  }
}

// ============ RelayServer methods ============

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
  config_.network.beacon = runFileConfig.beacon;

  log().info << "Configuration loaded";
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Beacon: " << config_.network.beacon;

  auto serverStarted = startFetchServer(config_.network.endpoint);
  if (!serverStarted) {
    return Service::Error(E_NETWORK, "Failed to start FetchServer: " +
                                         serverStarted.error().message);
  }

  // Initialize Relay with starting block id 0 (no beacon sync, no block
  // production)
  std::filesystem::path relayDataDir =
      std::filesystem::path(getWorkDir()) / DIR_DATA;
  Relay::InitConfig relayConfig;
  relayConfig.workDir = relayDataDir.string();
  relayConfig.timeOffset = 0;
  relayConfig.startingBlockId = 0;

  auto relayInit = relay_.init(relayConfig);
  if (!relayInit) {
    return Service::Error(E_RELAY, "Failed to initialize Relay: " +
                                       relayInit.error().message);
  }

  if (!config_.network.beacon.empty()) {
    auto syncResult = syncBlocksFromBeacon();
    if (!syncResult) {
      return Service::Error(E_NETWORK, "Failed to sync blocks from beacon: " +
                                           syncResult.error().message);
    }
    lastBlockSyncTime_ = std::chrono::steady_clock::now();
  }

  log().info << "Relay core initialized";
  log().info << "  Next block ID: " << relay_.getNextBlockId();

  initHandlers();
  log().info << "RelayServer initialization complete";
  return {};
}

RelayServer::Roe<void> RelayServer::syncBlocksFromBeacon() {
  if (config_.network.beacon.empty()) {
    return Error(E_CONFIG, "No beacon configured");
  }

  std::string beaconAddr = config_.network.beacon;
  log().info << "Syncing blocks from beacon: " << beaconAddr;

  if (!client_.setEndpoint(beaconAddr)) {
    return Error(E_CONFIG, "Failed to resolve beacon address: " + beaconAddr);
  }

  auto stateResult = client_.fetchBeaconState();
  if (!stateResult) {
    return Error(E_NETWORK,
                 "Failed to get beacon state: " + stateResult.error().message);
  }

  uint64_t latestBlockId = stateResult.value().nextBlockId;
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

  auto &hgb = requestHandlers_[Client::T_REQ_BLOCK_GET];
  hgb = [this](const Client::Request &request) { return hBlockGet(request); };

  auto &hga = requestHandlers_[Client::T_REQ_ACCOUNT_GET];
  hga = [this](const Client::Request &request) { return hAccountGet(request); };

  auto &hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab = [this](const Client::Request &request) { return hBlockAdd(request); };

  auto &hreg = requestHandlers_[Client::T_REQ_REGISTER];
  hreg = [this](const Client::Request &request) { return hRegister(request); };
};

void RelayServer::onStop() {
  Server::onStop();
  log().info << "RelayServer resources cleaned up";
}

void RelayServer::registerServer(const std::string &serverAddress) {
  // Get current timestamp
  int64_t now = std::chrono::system_clock::now().time_since_epoch().count();

  // Update or add server
  activeServers_[serverAddress] = now;
  log().debug << "Updated server record: " << serverAddress;
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

void RelayServer::syncBlocksPeriodically() {
  if (config_.network.beacon.empty()) {
    return;
  }
  auto now = std::chrono::steady_clock::now();
  auto elapsedSinceSync =
      std::chrono::duration_cast<std::chrono::seconds>(
          now - lastBlockSyncTime_);
  if (elapsedSinceSync >= BLOCK_SYNC_INTERVAL) {
    auto syncResult = syncBlocksFromBeacon();
    if (syncResult) {
      lastBlockSyncTime_ = now;
    } else {
      log().warning << "Periodic block sync failed: "
                    << syncResult.error().message;
    }
  }
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
  auto result = relay_.getBlock(blockId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get block: " + result.error().message);
  }

  return result.value().ltsToString();
}

RelayServer::Roe<std::string>
RelayServer::hBlockAdd(const Client::Request &request) {
  Ledger::ChainNode block;
  if (!block.ltsFromString(request.payload)) {
    return Error(E_REQUEST, "Failed to deserialize block: " + request.payload);
  }
  auto result = relay_.addBlock(block);
  if (!result) {
    return Error(E_REQUEST, "Failed to add block: " + result.error().message);
  }
  nlohmann::json resp;
  resp["message"] = "Block added";
  return resp.dump();
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
RelayServer::hRegister(const Client::Request &request) {
  network::TcpEndpoint endpoint =
      network::TcpEndpoint::ltsFromString(request.payload);
  registerServer(endpoint.ltsToString());
  return buildStateResponse().ltsToJson().dump();
}

RelayServer::Roe<std::string>
RelayServer::hStatus(const Client::Request &request) {
  return buildStateResponse().ltsToJson().dump();
}

RelayServer::Roe<std::string>
RelayServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST,
               "Unsupported request type: " + std::to_string(request.type));
}

} // namespace pp
