#include "MinerServer.h"
#include "../client/Client.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include "../ledger/Ledger.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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
  j["key"] = key;
  j["host"] = host;
  j["port"] = port;
  j["beacons"] = beacons;
  return j;
}

MinerServer::Roe<void> MinerServer::RunFileConfig::ltsFromJson(const nlohmann::json& jd) {
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

    // Load and validate key file (required)
    if (!jd.contains("key")) {
      return Error(E_CONFIG, "Field 'key' is required");
    }
    if (!jd["key"].is_string()) {
      return Error(E_CONFIG, "Field 'key' must be a string");
    }
    key = jd["key"].get<std::string>();
    if (key.empty()) {
      return Error(E_CONFIG, "Field 'key' cannot be empty");
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
      return Error(E_CONFIG, "Field 'beacons' array must contain at least one beacon address");
    }

    beacons.clear();
    for (size_t i = 0; i < jd["beacons"].size(); ++i) {
      const auto& beacon = jd["beacons"][i];
      if (!beacon.is_string()) {
        return Error(E_CONFIG, "All elements in 'beacons' array must be strings (index " + std::to_string(i) + " is not)");
      }
      std::string beaconAddr = beacon.get<std::string>();
      if (beaconAddr.empty()) {
        return Error(E_CONFIG, "Beacon address at index " + std::to_string(i) + " cannot be empty");
      }
      beacons.push_back(beaconAddr);
    }

    if (beacons.empty()) {
      return Error(E_CONFIG, "Field 'beacons' array must contain at least one valid string address");
    }

    return {};
  } catch (const std::exception& e) {
    return Error(E_CONFIG, "Failed to parse run configuration: " + std::string(e.what()));
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
    log().info << "No " << FILE_CONFIG << " found, creating with default values";
    
    // Use default values from RunFileConfig struct
    nlohmann::json defaultConfig = runFileConfig.ltsToJson();
    
    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_MINER, "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << defaultConfig.dump(2) << std::endl;
    configFile.close();
    
    log().info << "Created " << FILE_CONFIG << " at: " << configPathStr;
    log().info << "Please edit " << FILE_CONFIG << " to configure your miner settings";
  } else {
    // Load existing configuration
    auto jsonResult = utl::loadJsonFile(configPathStr);
    if (!jsonResult) {
      return Service::Error(E_CONFIG, "Failed to load config file: " + jsonResult.error().message);
    }
    
    nlohmann::json config = jsonResult.value();
    auto parseResult = runFileConfig.ltsFromJson(config);
    if (!parseResult) {
      return Service::Error(E_CONFIG, "Failed to parse config file: " + parseResult.error().message);
    }
  }

  // Apply configuration from RunFileConfig
  config_.minerId = runFileConfig.minerId;
  config_.privateKey = utl::readKey(runFileConfig.key);
  config_.network.endpoint.address = runFileConfig.host;
  config_.network.endpoint.port = runFileConfig.port;
  config_.network.beacons = runFileConfig.beacons;
  
  log().info << "Configuration loaded";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Beacons: " << config_.network.beacons.size();
  
  auto serverStarted = startFetchServer(config_.network.endpoint);
  if (!serverStarted) {
    return Service::Error(E_MINER, "Failed to start FetchServer: " + serverStarted.error().message);
  }

  // Connect to beacon server and fetch initial state
  auto beaconResult = connectToBeacon();
  if (!beaconResult) {
    return Service::Error(E_NETWORK, "Failed to connect to beacon: " + beaconResult.error().message);
  }
  log().info << "Successfully connected to beacon and synchronized initial state";
  const auto& state = beaconResult.value();

  // Initialize miner core
  std::filesystem::path minerDataDir = std::filesystem::path(getWorkDir()) / DIR_DATA;
  
  Miner::InitConfig minerConfig;
  minerConfig.minerId = config_.minerId;
  minerConfig.privateKey = config_.privateKey;
  minerConfig.timeOffset = state.currentTimestamp - utl::getCurrentTime();
  minerConfig.workDir = minerDataDir.string();
  minerConfig.startingBlockId = state.lastCheckpointId;
  minerConfig.checkpointId = state.checkpointId;

  
  auto minerInit = miner_.init(minerConfig);
  if (!minerInit) {
    return Service::Error(E_MINER, "Failed to initialize Miner: " + minerInit.error().message);
  }
  
  auto syncResult = syncBlocksFromBeacon();
  if (!syncResult) {
    return Service::Error(E_MINER, "Failed to sync blocks from beacon: " + syncResult.error().message);
  }

  log().info << "Miner core initialized";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake: " << miner_.getStake();

  log().info << "MinerServer initialization complete";
  return {};
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

  auto stateResult = client_.fetchBeaconState();
  if (!stateResult) {
    return Error(E_NETWORK, "Failed to get beacon state: " + stateResult.error().message);
  }

  uint64_t latestBlockId = stateResult.value().nextBlockId;
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
                   "Failed to fetch block " + std::to_string(blockId) + " from beacon: " +
                       blockResult.error().message);
    }

    Ledger::ChainNode block = blockResult.value();
    block.hash = miner_.calculateHash(block.block);

    auto addResult = miner_.addBlock(block);
    if (!addResult) {
      return Error(E_MINER,
                   "Failed to add block " + std::to_string(blockId) + ": " + addResult.error().message);
    }

    log().debug << "Synced block " << blockId;
  }

  log().info << "Sync complete: " << (latestBlockId - nextBlockId) << " blocks added";

  initHandlers();

  return {};
}

void MinerServer::initHandlers() {
  requestHandlers_.clear();

  auto& hgs = requestHandlers_[Client::T_REQ_STATUS];
  hgs = [this](const Client::Request &request) { return hStatus(request); };

  auto& hgb = requestHandlers_[Client::T_REQ_BLOCK_GET];
  hgb = [this](const Client::Request &request) { return hBlockGet(request); };

  auto& hga = requestHandlers_[Client::T_REQ_ACCOUNT_GET];
  hga = [this](const Client::Request &request) { return hAccountGet(request); };

  auto& hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab = [this](const Client::Request &request) { return hBlockAdd(request); };

  auto& hta = requestHandlers_[Client::T_REQ_TRANSACTION_ADD];
  hta = [this](const Client::Request &request) { return hTransactionAdd(request); };
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

      if (miner_.isSlotLeader()) {
        handleSlotLeaderRole();
      } else {
        handleValidatorRole();
      }
      
      // Sleep for a short time before checking again
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      
    } catch (const std::exception& e) {
      log().error << "Exception in block production loop: " << e.what();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  
  log().info << "Block production and request handler loop stopped";
}

std::string MinerServer::getSlotLeaderAddress() const {
  // TODO: Implement slot leader address selection
  return "";
}

std::string MinerServer::handleParsedRequest(const Client::Request& request) {
  log().debug << "Handling request: " << request.type;
  auto it = requestHandlers_.find(request.type);
  Roe<std::string> result = (it != requestHandlers_.end()) ? it->second(request) : hUnsupported(request);
  if (!result) {
    return Server::packResponse(1, result.error().message);
  }
  return Server::packResponse(result.value());
}

MinerServer::Roe<std::string> MinerServer::hBlockGet(const Client::Request &request) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    return Error(E_REQUEST, "Invalid block get payload: " + request.payload);
  }
  uint64_t blockId = idResult.value();
  auto result = miner_.getBlock(blockId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get block: " + result.error().message);
  }
  return result.value().ltsToString();
}

MinerServer::Roe<std::string> MinerServer::hBlockAdd(const Client::Request &request) {
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

MinerServer::Roe<std::string> MinerServer::hAccountGet(const Client::Request &request) {
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

MinerServer::Roe<std::string> MinerServer::hTransactionAdd(const Client::Request &request) {
  auto signedTxResult = utl::binaryUnpack<Ledger::SignedData<Ledger::Transaction>>(request.payload);
  if (!signedTxResult) {
    return Error(E_REQUEST, "Failed to deserialize transaction: " + signedTxResult.error().message);
  }

  const auto& signedTx = signedTxResult.value();
  if (miner_.isSlotLeader()) {
    auto result = miner_.addTransaction(signedTx);
    if (!result) {
      return Error(E_REQUEST, result.error().message);
    }
    return {"Transaction added to pool"};
  }

  std::string leaderAddr = getSlotLeaderAddress();
  if (!client_.setEndpoint(leaderAddr)) {
    return Error(E_CONFIG, "Failed to resolve leader address: " + leaderAddr);
  }

  auto result = client_.addTransaction(signedTx);
  if (!result) {
    return Error(E_REQUEST, result.error().message);
  }
  return {"Transaction submitted to slot leader"};
}

MinerServer::Roe<std::string> MinerServer::hStatus(const Client::Request &request) {
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

MinerServer::Roe<std::string> MinerServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST, "Unsupported request type: " + std::to_string(request.type));
}

void MinerServer::handleSlotLeaderRole() {
  if (miner_.shouldProduceBlock()) {
    log().info << "Attempting to produce block for slot " << miner_.getCurrentSlot();
    
    auto result = miner_.produceBlock();
    if (result) {
      auto block = result.value();
      log().info << "Successfully produced block " << block.block.index 
                << " with hash " << block.hash;
      
      auto broadcastResult = broadcastBlock(block);
      if (broadcastResult) {
        log().info << "Block broadcasted";
        miner_.confirmProducedBlock(block);
        miner_.addBlock(block);  // Add to own ledger
      } else {
        log().warning << "Failed to broadcast block: " + broadcastResult.error().message;
      }
    } else {
      log().warning << "Failed to produce block: " << result.error().message;
    }
  }
}

void MinerServer::handleValidatorRole() {
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

  network::TcpEndpoint endpoint = getFetchServerEndpoint();
  auto stateResult = client_.registerMinerServer(endpoint);
  if (!stateResult) {
    return Error(E_NETWORK, "Failed to get beacon state: " + stateResult.error().message);
  }

  const auto& state = stateResult.value();
  log().info << "Latest checkpoint ID: " << state.checkpointId;
  log().info << "Next block ID: " << state.nextBlockId;

  return state;
}

MinerServer::Roe<void> MinerServer::broadcastBlock(const Ledger::ChainNode& block) {
  bool anySuccess = false;
  for (const auto& beacon : config_.network.beacons) {
    if (!client_.setEndpoint(beacon)) {
      log().warning << "Failed to resolve beacon address: " + beacon;
      continue;
    }
    auto clientResult = client_.addBlock(block);
    if (!clientResult) {
      log().warning << "Failed to add block to beacon: " + beacon + ": " + clientResult.error().message;
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
