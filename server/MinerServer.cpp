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
  fetchServer_.redirectLogger(log().getFullName() + ".FetchServer");
  client_.redirectLogger(log().getFullName() + ".Client");
}

// ============ RunFileConfig methods ============

nlohmann::json MinerServer::RunFileConfig::ltsToJson() {
  nlohmann::json j;
  j["minerId"] = minerId;
  j["tokenId"] = tokenId;
  j["privateKey"] = privateKey;
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

    // Load and validate tokenId (optional, defaults to ID_GENESIS)
    if (jd.contains("tokenId")) {
      if (!jd["tokenId"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'tokenId' must be a non-negative number");
      }
      tokenId = jd["tokenId"].get<uint64_t>();
    } else {
      tokenId = AccountBuffer::ID_GENESIS;
    }

    // Load and validate privateKey (required)
    if (!jd.contains("privateKey")) {
      return Error(E_CONFIG, "Field 'privateKey' is required");
    }
    if (!jd["privateKey"].is_string()) {
      return Error(E_CONFIG, "Field 'privateKey' must be a string");
    }
    privateKey = jd["privateKey"].get<std::string>();
    if (privateKey.empty()) {
      return Error(E_CONFIG, "Field 'privateKey' cannot be empty");
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

Service::Roe<void> MinerServer::run(const std::string &workDir) {
  // Store dataDir for onStart
  workDir_ = workDir;

  auto signaturePath = std::filesystem::path(workDir) / FILE_SIGNATURE;
  if (!std::filesystem::exists(workDir)) {
    std::filesystem::create_directories(workDir);
    auto result = utl::writeToNewFile(signaturePath.string(), "");
    if (!result) {
      return Service::Error(E_MINER, "Failed to create signature file: " + result.error().message);
    }
  }

  if (!std::filesystem::exists(signaturePath)) {
    return Service::Error(E_MINER, "Work directory not recognized, please remove it manually and try again");
  }

  log().info << "Running MinerServer with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + FILE_LOG, logging::getLevel());

  // Call base class run which will invoke onStart() then runLoop() in current thread
  return Service::run();
}

Service::Roe<void> MinerServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(workDir_) / FILE_CONFIG;
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
  config_.tokenId = runFileConfig.tokenId;
  config_.privateKey = runFileConfig.privateKey;
  config_.network.endpoint.address = runFileConfig.host;
  config_.network.endpoint.port = runFileConfig.port;
  config_.network.beacons = runFileConfig.beacons;
  
  log().info << "Configuration loaded";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Beacons: " << config_.network.beacons.size();
  
  // Start FetchServer with handler
  network::FetchServer::Config fetchServerConfig;
  fetchServerConfig.endpoint = config_.network.endpoint;
  fetchServerConfig.handler = [this](const std::string &request, std::shared_ptr<network::TcpConnection> conn) {
    std::string response = handleRequest(request);
    conn->send(response);
    conn->close();
  };
  auto serverStarted = fetchServer_.start(fetchServerConfig);

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
  std::filesystem::path minerDataDir = std::filesystem::path(workDir_) / DIR_DATA;
  
  Miner::InitConfig minerConfig;
  minerConfig.minerId = config_.minerId;
  minerConfig.tokenId = config_.tokenId;
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
  return {};
}

void MinerServer::onStop() {
  fetchServer_.stop();
  log().info << "MinerServer resources cleaned up";
}

void MinerServer::runLoop() {
  log().info << "Block production loop started";
  
  while (!isStopSet()) {
    try {
      miner_.refreshStakeholders();

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
  
  log().info << "Block production loop stopped";
}

std::string MinerServer::getSlotLeaderAddress() const {
  // TODO: Implement slot leader address selection
  return "";
}

std::string MinerServer::handleRequest(const std::string &request) {
  log().debug << "Received request (" << request.size() << " bytes)";
  auto reqResult = utl::binaryUnpack<Client::Request>(request);
  if (!reqResult) {
    return binaryResponseError(1, reqResult.error().message);
  }

  const auto& req = reqResult.value();
  auto result = handleRequest(req);
  if (!result) {
    return binaryResponseError(1, result.error().message);
  }
  return binaryResponseOk(result.value());
}

MinerServer::Roe<std::string> MinerServer::handleRequest(const Client::Request &request) {
  switch (request.type) {
  case Client::T_REQ_BLOCK_GET:
    return handleBlockGetRequest(request);
  case Client::T_REQ_BLOCK_ADD:
    return handleBlockAddRequest(request);
  case Client::T_REQ_TRANSACTION_ADD:
    return handleTransactionAddRequest(request);
  case Client::T_REQ_JSON:
    return handleJsonRequest(request.payload);
  default:
    return Error(E_REQUEST, "Unknown request type: " + std::to_string(request.type));
  }
}

MinerServer::Roe<std::string> MinerServer::handleBlockGetRequest(const Client::Request &request) {
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

MinerServer::Roe<std::string> MinerServer::handleBlockAddRequest(const Client::Request &request) {
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

MinerServer::Roe<std::string> MinerServer::handleJsonRequest(const std::string &payload) {
  auto jsonResult = utl::parseJsonRequest(payload);
  if (jsonResult.isError()) {
    return Error(E_REQUEST, "Failed to parse request JSON: " + jsonResult.error().message);
  }
  return handleJsonRequest(jsonResult.value());
}

MinerServer::Roe<std::string> MinerServer::handleJsonRequest(const nlohmann::json &reqJson) {
  if (!reqJson.contains("type")) {
    return Error(E_REQUEST, "Missing type field in request JSON");
  }
  std::string type = reqJson["type"].get<std::string>();

  if (type == "checkpoint") {
    return handleCheckpointRequest(reqJson);
  } else if (type == "consensus") {
    return handleConsensusRequest(reqJson);
  } else if (type == "status") {
    return handleStatusRequest(reqJson);
  } else {
    return Error(E_REQUEST, "Unknown request type: " + type);
  }
}

MinerServer::Roe<std::string> MinerServer::handleTransactionAddRequest(const Client::Request &request) {
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

MinerServer::Roe<std::string> MinerServer::handleCheckpointRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("action")) {
    return Error(E_REQUEST, "missing action field");
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  nlohmann::json resp;
  if (action == "isOutOfDate") {
    if (!reqJson.contains("checkpointId")) {
      return Error(E_REQUEST, "missing checkpointId field");
    }
    
    uint64_t checkpointId = reqJson["checkpointId"].get<uint64_t>();
    resp["isOutOfDate"] = miner_.isOutOfDate(checkpointId);
  } else {
    return Error(E_REQUEST, "unknown checkpoint action: " + action);
  }
  
  return resp.dump();
}

MinerServer::Roe<std::string> MinerServer::handleConsensusRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("action")) {
    return Error(E_REQUEST, "missing action field");
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  nlohmann::json resp;
  if (action == "currentSlot") {
    resp["currentSlot"] = miner_.getCurrentSlot();
  } else if (action == "currentEpoch") {
    resp["currentEpoch"] = miner_.getCurrentEpoch();
  } else if (action == "isSlotLeader") {
    if (reqJson.contains("slot")) {
      uint64_t slot = reqJson["slot"].get<uint64_t>();
      resp["isSlotLeader"] = miner_.isSlotLeader(slot);
    } else {
      resp["isSlotLeader"] = miner_.isSlotLeader();
      resp["currentSlot"] = miner_.getCurrentSlot();
    }
  } else {
    return Error(E_REQUEST, "unknown consensus action: " + action);
  }
  
  return resp.dump();
}

MinerServer::Roe<std::string> MinerServer::handleStatusRequest(const nlohmann::json& reqJson) {
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

  network::TcpEndpoint endpoint;
  endpoint.address = fetchServer_.getHost();
  endpoint.port = fetchServer_.getPort();
  auto stateResult = client_.registerMinerServer(endpoint);
  if (!stateResult) {
    return Error(E_NETWORK, "Failed to get beacon state: " + stateResult.error().message);
  }

  const auto& state = stateResult.value();
  log().info << "Latest checkpoint ID: " << state.checkpointId;
  log().info << "Next block ID: " << state.nextBlockId;

  return state;
}

std::string MinerServer::binaryResponseOk(const std::string& payload) const {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = 0;
  resp.payload = payload;
  return utl::binaryPack(resp);
}

std::string MinerServer::binaryResponseError(uint16_t errorCode, const std::string& message) const {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = errorCode;
  resp.payload = message;
  return utl::binaryPack(resp);
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
