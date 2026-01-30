#include "MinerServer.h"
#include "../client/Client.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pp {

MinerServer::MinerServer() {
  redirectLogger("MinerServer");
  miner_.redirectLogger(log().getFullName() + ".Miner");
  fetchServer_.redirectLogger(log().getFullName() + ".FetchServer");
}

MinerServer::~MinerServer() {
  if (isRunning()) {
    Service::stop();
  }
}

Service::Roe<void> MinerServer::start(const std::string &workDir) {
  if (isRunning()) {
    return Service::Error(E_MINER, "MinerServer is already running");
  }

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

  log().info << "Starting MinerServer with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + FILE_LOG, logging::Level::DEBUG);

  // Call base class start which will invoke onStart() then run()
  return Service::start();
}

Service::Roe<void> MinerServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(workDir_) / FILE_CONFIG;
  std::string configPathStr = configPath.string();

  // Create default FILE_CONFIG if it doesn't exist
  if (!std::filesystem::exists(configPath)) {
    log().info << "No " << FILE_CONFIG << " found, creating with default values";
    
    nlohmann::json defaultConfig;
    defaultConfig["minerId"] = 0;
    defaultConfig["host"] = Client::DEFAULT_HOST;
    defaultConfig["port"] = Client::DEFAULT_MINER_PORT;
    // Default beacon address - user should edit this
    defaultConfig["beacons"] = nlohmann::json::array({});
    
    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_MINER, "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << defaultConfig.dump(2) << std::endl;
    configFile.close();
    
    log().info << "Created " << FILE_CONFIG << " at: " << configPathStr;
    log().info << "Please edit " << FILE_CONFIG << " to configure your miner settings";
  }

  // Load configuration
  auto configResult = loadConfig(configPathStr);
  if (!configResult) {
    return Service::Error(E_MINER, "Failed to load configuration: " + configResult.error().message);
  }
  
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

  // Initialize miner core
  std::filesystem::path minerDataDir = std::filesystem::path(workDir_) / DIR_DATA;
  
  Miner::InitConfig minerConfig;
  minerConfig.minerId = config_.minerId;
  minerConfig.workDir = minerDataDir.string();
  
  auto minerInit = miner_.init(minerConfig);
  if (!minerInit) {
    return Service::Error(E_MINER, "Failed to initialize Miner: " + minerInit.error().message);
  }
  
  log().info << "Miner core initialized";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake: " << miner_.getStake();

  log().info << "MinerServer initialization complete";
  return {};
}

void MinerServer::onStop() {
  fetchServer_.stop();
  log().info << "MinerServer resources cleaned up";
}

void MinerServer::run() {
  log().info << "Block production loop started";
  
  while (isRunning()) {
    try {
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

MinerServer::Roe<void> MinerServer::loadConfig(const std::string &configPath) {
  // Use the utility function for loading JSON
  auto jsonResult = utl::loadJsonFile(configPath);
  if (jsonResult.isError()) {
    // Convert pp::Error to MinerServer::Error
    return Error(E_CONFIG, jsonResult.error().message);
  }

  nlohmann::json config = jsonResult.value();

  // Load miner ID (required)
  if (!config.contains("minerId") || !config["minerId"].is_number()) {
    return Error(E_CONFIG, "Configuration file missing 'minerId' field");
  }
  config_.minerId = config["minerId"].get<uint64_t>();

  // Load host (optional, default: "localhost")
  if (config.contains("host") && config["host"].is_string()) {
    config_.network.endpoint.address = config["host"].get<std::string>();
  } else {
    config_.network.endpoint.address = Client::DEFAULT_HOST;
  }

  // Load port (optional, default: 8518)
  if (config.contains("port") && config["port"].is_number()) {
    if (config["port"].is_number_integer()) {
      config_.network.endpoint.port = config["port"].get<uint16_t>();
    } else {
      return Error(E_CONFIG, "Configuration file 'port' field is not an integer");
    }
  } else {
    config_.network.endpoint.port = 8518; // Default miner port
  }

  // Load beacon addresses (required)
  if (!config.contains("beacons")) {
    return Error(E_CONFIG, "Configuration file missing required 'beacons' field");
  }
  
  if (!config["beacons"].is_array()) {
    return Error(E_CONFIG, "Configuration file 'beacons' field must be an array");
  }
  
  if (config["beacons"].empty()) {
    return Error(E_CONFIG, "Configuration file 'beacons' array must contain at least one beacon address");
  }
  
  for (const auto &beaconAddr : config["beacons"]) {
    if (beaconAddr.is_string()) {
      std::string addr = beaconAddr.get<std::string>();
      config_.network.beacons.push_back(addr);
      log().info << "Found beacon address in config: " << addr;
    }
  }
  
  if (config_.network.beacons.empty()) {
    return Error(E_CONFIG, "Configuration file 'beacons' array must contain at least one valid string address");
  }

  log().info << "Configuration loaded from " << configPath;
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Beacons: " << config_.network.beacons.size();
  return {};
}

std::string MinerServer::handleRequest(const std::string &request) {
  log().debug << "Received request (" << request.size() << " bytes)";

  // Use the utility function for parsing JSON request
  auto jsonResult = utl::parseJsonRequest(request);
  if (jsonResult.isError()) {
    nlohmann::json errorResp;
    errorResp["error"] = jsonResult.error().message;
    return errorResp.dump();
  }
  
  nlohmann::json reqJson = jsonResult.value();
  std::string type = reqJson["type"].get<std::string>();

  if (type == "transaction") {
    return handleTransactionRequest(reqJson);
  } else if (type == "block") {
    return handleBlockRequest(reqJson);
  } else if (type == "mining") {
    return handleMiningRequest(reqJson);
  } else if (type == "checkpoint") {
    return handleCheckpointRequest(reqJson);
  } else if (type == "consensus") {
    return handleConsensusRequest(reqJson);
  } else if (type == "status") {
    return handleStatusRequest(reqJson);
  } else {
    nlohmann::json resp;
    resp["error"] = "unknown request type: " + type;
    return resp.dump();
  }
}

std::string MinerServer::handleTransactionRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "add") {
    if (!reqJson.contains("transaction")) {
      resp["error"] = "missing transaction field";
      return resp.dump();
    }
    
    auto& txJson = reqJson["transaction"];
    Ledger::Transaction tx;
    
    if (!txJson.contains("from") || !txJson.contains("to") || !txJson.contains("amount")) {
      resp["error"] = "missing required transaction fields (from, to, amount)";
      return resp.dump();
    }
    
    tx.fromWalletId = txJson["from"].get<uint64_t>();
    tx.toWalletId = txJson["to"].get<uint64_t>();
    tx.amount = txJson["amount"].get<int64_t>();
    
    auto result = miner_.addTransaction(tx);
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    resp["status"] = "ok";
    resp["message"] = "Transaction added to pool";
    
  } else if (action == "count") {
    resp["status"] = "ok";
    resp["pendingTransactions"] = miner_.getPendingTransactionCount();
    
  } else if (action == "clear") {
    miner_.clearTransactionPool();
    resp["status"] = "ok";
    resp["message"] = "Transaction pool cleared";
    
  } else {
    resp["error"] = "unknown transaction action: " + action;
  }
  
  return resp.dump();
}

std::string MinerServer::handleBlockRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "get") {
    if (!reqJson.contains("blockId")) {
      resp["error"] = "missing blockId field";
      return resp.dump();
    }
    
    uint64_t blockId = reqJson["blockId"].get<uint64_t>();
    auto result = miner_.getBlock(blockId);
    
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    const Ledger::ChainNode& block = result.value();
    resp["status"] = "ok";
    resp["block"] = utl::toJsonSafeString(block.ltsToString());

  } else if (action == "add") {
    if (!reqJson.contains("block")) {
      resp["error"] = "missing block field";
      return resp.dump();
    }
    if (!reqJson["block"].is_string()) {
      resp["error"] = "block field must be hex string";
      return resp.dump();
    }
    std::string hex = utl::fromJsonSafeString(reqJson["block"].get<std::string>());
    Ledger::ChainNode block;
    if (!block.ltsFromString(hex)) {
      resp["error"] = "Failed to deserialize block";
      return resp.dump();
    }

    // Calculate hash if not provided or if we want to verify/override
    block.hash = miner_.calculateHash(block.block);
    
    auto result = miner_.addBlock(block);
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    resp["status"] = "ok";
    resp["message"] = "Block added successfully";
    
  } else if (action == "current") {
    resp["status"] = "ok";
    resp["currentBlockId"] = miner_.getCurrentBlockId();
    
  } else {
    resp["error"] = "unknown block action: " + action;
  }
  
  return resp.dump();
}

std::string MinerServer::handleMiningRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "produce") {
    if (!miner_.isSlotLeader()) {
      resp["error"] = "not slot leader for current slot";
      return resp.dump();
    }
    
    auto result = miner_.produceBlock();
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    auto block = result.value();
    resp["status"] = "ok";
    resp["message"] = "Block produced";
    resp["block"]["index"] = block->block.index;
    resp["block"]["hash"] = block->hash;
    resp["block"]["slot"] = block->block.slot;
    
  } else if (action == "shouldProduce") {
    resp["status"] = "ok";
    resp["shouldProduce"] = miner_.isSlotLeader();
    resp["currentSlot"] = miner_.getCurrentSlot();
    
  } else {
    resp["error"] = "unknown mining action: " + action;
  }
  
  return resp.dump();
}

std::string MinerServer::handleCheckpointRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "isOutOfDate") {
    if (!reqJson.contains("checkpointId")) {
      resp["error"] = "missing checkpointId field";
      return resp.dump();
    }
    
    uint64_t checkpointId = reqJson["checkpointId"].get<uint64_t>();
    resp["status"] = "ok";
    resp["isOutOfDate"] = miner_.isOutOfDate(checkpointId);
    
  } else {
    resp["error"] = "unknown checkpoint action: " + action;
  }
  
  return resp.dump();
}

std::string MinerServer::handleConsensusRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "currentSlot") {
    resp["status"] = "ok";
    resp["currentSlot"] = miner_.getCurrentSlot();
    
  } else if (action == "currentEpoch") {
    resp["status"] = "ok";
    resp["currentEpoch"] = miner_.getCurrentEpoch();
    
  } else if (action == "isSlotLeader") {
    if (reqJson.contains("slot")) {
      uint64_t slot = reqJson["slot"].get<uint64_t>();
      resp["status"] = "ok";
      resp["isSlotLeader"] = miner_.isSlotLeader(slot);
    } else {
      resp["status"] = "ok";
      resp["isSlotLeader"] = miner_.isSlotLeader();
      resp["currentSlot"] = miner_.getCurrentSlot();
    }
    
  } else {
    resp["error"] = "unknown consensus action: " + action;
  }
  
  return resp.dump();
}

std::string MinerServer::handleStatusRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  resp["status"] = "ok";
  resp["minerId"] = config_.minerId;
  resp["stake"] = miner_.getStake();
  resp["currentBlockId"] = miner_.getCurrentBlockId();
  resp["currentSlot"] = miner_.getCurrentSlot();
  resp["currentEpoch"] = miner_.getCurrentEpoch();
  resp["pendingTransactions"] = miner_.getPendingTransactionCount();
  resp["isSlotLeader"] = miner_.shouldProduceBlock();
  
  return resp.dump();
}

void MinerServer::handleSlotLeaderRole() {
  if (miner_.shouldProduceBlock()) {
    log().info << "Attempting to produce block for slot " << miner_.getCurrentSlot();
    
    auto result = miner_.produceBlock();
    if (result) {
      auto block = result.value();
      log().info << "Successfully produced block " << block->block.index 
                << " with hash " << block->hash;
      
      // In a full implementation, we would broadcast this block to beacons
      // and other miners here
    } else {
      log().warning << "Failed to produce block: " << result.error().message;
    }
  }
}

void MinerServer::handleValidatorRole() {
  // Not slot leader - act as validator
  // Monitor for new blocks from other miners and validate them
  log().debug << "Acting as validator for slot " << miner_.getCurrentSlot();
  
  // In a full implementation, we would:
  // 1. Listen for blocks from the current slot leader
  // 2. Validate received blocks
  // 3. Add valid blocks to our chain
  // 4. Participate in consensus voting if required
  
  // For now, this is a placeholder for validator behavior
  // The actual block reception would happen via network requests
}

MinerServer::Roe<void> MinerServer::connectToBeacon() {
  if (config_.network.beacons.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
  }

  // Try to connect to the first beacon in the list
  std::string beaconAddr = config_.network.beacons[0];
  log().info << "Connecting to beacon server: " << beaconAddr;

  Client client;
  client.redirectLogger(log().getFullName() + ".Client");
  if (!client.setEndpoint(beaconAddr)) {
    return Error(E_CONFIG, "Failed to resolve beacon address: " + beaconAddr);
  }

  auto stateResult = client.fetchBeaconState();
  if (!stateResult) {
    return Error(E_NETWORK, "Failed to get beacon state: " + stateResult.error().message);
  }

  const auto& state = stateResult.value();
  log().info << "Latest checkpoint ID: " << state.checkpointId;
  log().info << "Latest block ID: " << state.blockId;
  log().info << "Retrieved " << state.stakeholders.size() << " stakeholders from beacon";

  // Register stakeholders with the miner's consensus module
  // Note: We need to access the consensus module through the miner
  // For now, we'll just log the stakeholders
  uint64_t totalStake = 0;
  for (const auto& sh : state.stakeholders) {
    log().info << "  Stakeholder: " << sh.id << " with stake " << sh.stake;
    totalStake += sh.stake;
  }
  log().info << "Total stake in network: " << totalStake;

  return {};
}

} // namespace pp
