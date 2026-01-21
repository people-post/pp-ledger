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
  setLogger("MinerServer");
  log().info << "MinerServer initialized";
}

MinerServer::~MinerServer() {
  if (isRunning()) {
    Service::stop();
  }
}

bool MinerServer::start(const std::string &dataDir) {
  if (isRunning()) {
    log().warning << "MinerServer is already running";
    return false;
  }

  // Store dataDir for onStart
  dataDir_ = dataDir;

  log().info << "Starting MinerServer with work directory: " << dataDir;
  log().addFileHandler(dataDir + "/miner.log", logging::Level::DEBUG);

  // Call base class start which will invoke onStart() then run()
  return Service::start();
}

bool MinerServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(dataDir_) / "config.json";
  std::string configPathStr = configPath.string();

  // Load configuration
  auto configResult = loadConfig(configPathStr);
  if (!configResult) {
    log().error << "Failed to load configuration: "
                << configResult.error().message;
    return false;
  }
  
  // Initialize miner core
  Miner::Config minerConfig;
  minerConfig.minerId = config_.minerId;
  minerConfig.stake = config_.stake;
  minerConfig.workDir = dataDir_;
  minerConfig.slotDuration = 1;
  minerConfig.slotsPerEpoch = 21600;
  minerConfig.maxPendingTransactions = 10000;
  minerConfig.maxTransactionsPerBlock = 100;
  
  auto minerInit = miner_.init(minerConfig);
  if (!minerInit) {
    log().error << "Failed to initialize Miner: " << minerInit.error().message;
    return false;
  }
  
  log().info << "Miner core initialized";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake: " << config_.stake;

  // Start FetchServer with handler
  network::FetchServer::Config fetchServerConfig;
  fetchServerConfig.endpoint = config_.network.endpoint;
  fetchServerConfig.handler = [this](const std::string &request, std::shared_ptr<network::TcpConnection> conn) {
    std::string response = handleRequest(request);
    conn->send(response);
    conn->close();
  };
  bool serverStarted = fetchServer_.start(fetchServerConfig);

  if (!serverStarted) {
    log().error << "Failed to start FetchServer";
    return false;
  }

  log().info << "MinerServer initialization complete";
  return true;
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
  if (!std::filesystem::exists(configPath)) {
    return Error(1, "Configuration file not found: " + configPath);
  }

  std::ifstream configFile(configPath);
  if (!configFile.is_open()) {
    return Error(2, "Failed to open configuration file: " + configPath);
  }

  // Read file content
  std::string content((std::istreambuf_iterator<char>(configFile)),
                      std::istreambuf_iterator<char>());
  configFile.close();

  // Parse JSON
  nlohmann::json config;
  try {
    config = nlohmann::json::parse(content);
  } catch (const nlohmann::json::parse_error &e) {
    return Error(3, "Failed to parse JSON: " + std::string(e.what()));
  }

  // Load miner ID (required)
  if (!config.contains("minerId") || !config["minerId"].is_string()) {
    return Error(4, "Configuration file missing 'minerId' field");
  }
  config_.minerId = config["minerId"].get<std::string>();

  // Load stake (required)
  if (!config.contains("stake") || !config["stake"].is_number()) {
    return Error(5, "Configuration file missing 'stake' field");
  }
  config_.stake = config["stake"].get<uint64_t>();

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
      return Error(6, "Configuration file 'port' field is not an integer");
    }
  } else {
    config_.network.endpoint.port = 8518; // Default miner port
  }

  // Load beacon addresses (optional)
  if (config.contains("beacons") && config["beacons"].is_array()) {
    for (const auto &beaconAddr : config["beacons"]) {
      if (beaconAddr.is_string()) {
        std::string addr = beaconAddr.get<std::string>();
        config_.network.beacons.push_back(addr);
        log().info << "Found beacon address in config: " << addr;
      }
    }
  }

  log().info << "Configuration loaded from " << configPath;
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Beacons: " << config_.network.beacons.size();
  return {};
}

std::string MinerServer::handleRequest(const std::string &request) {
  log().debug << "Received request (" << request.size() << " bytes)";

  // Try to parse as JSON
  nlohmann::json reqJson;
  try {
    reqJson = nlohmann::json::parse(request);
  } catch (const nlohmann::json::parse_error &e) {
    log().error << "Failed to parse request JSON: " << e.what();
    nlohmann::json errorResp;
    errorResp["error"] = "invalid json";
    return errorResp.dump();
  }

  // Handle different request types
  if (!reqJson.contains("type")) {
    nlohmann::json resp;
    resp["error"] = "missing type field";
    return resp.dump();
  }
  
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
    nlohmann::json resp;
    resp["status"] = "ok";
    resp["minerId"] = config_.minerId;
    resp["stake"] = config_.stake;
    resp["currentBlockId"] = miner_.getCurrentBlockId();
    resp["currentSlot"] = miner_.getCurrentSlot();
    resp["currentEpoch"] = miner_.getCurrentEpoch();
    resp["pendingTransactions"] = miner_.getPendingTransactionCount();
    resp["isSlotLeader"] = miner_.shouldProduceBlock();
    return resp.dump();
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
    
    tx.fromWallet = txJson["from"].get<std::string>();
    tx.toWallet = txJson["to"].get<std::string>();
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
    
    auto block = result.value();
    resp["status"] = "ok";
    resp["block"]["index"] = block->getIndex();
    resp["block"]["timestamp"] = block->getTimestamp();
    resp["block"]["hash"] = block->getHash();
    resp["block"]["previousHash"] = block->getPreviousHash();
    resp["block"]["slot"] = block->getSlot();
    resp["block"]["slotLeader"] = block->getSlotLeader();
    
  } else if (action == "add") {
    if (!reqJson.contains("block")) {
      resp["error"] = "missing block field";
      return resp.dump();
    }
    
    auto& blockJson = reqJson["block"];
    Block block;
    
    if (blockJson.contains("index")) block.setIndex(blockJson["index"].get<uint64_t>());
    if (blockJson.contains("timestamp")) block.setTimestamp(blockJson["timestamp"].get<int64_t>());
    if (blockJson.contains("data")) block.setData(blockJson["data"].get<std::string>());
    if (blockJson.contains("previousHash")) block.setPreviousHash(blockJson["previousHash"].get<std::string>());
    if (blockJson.contains("slot")) block.setSlot(blockJson["slot"].get<uint64_t>());
    if (blockJson.contains("slotLeader")) block.setSlotLeader(blockJson["slotLeader"].get<std::string>());
    
    block.calculateHash();
    
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
    resp["block"]["index"] = block->getIndex();
    resp["block"]["hash"] = block->getHash();
    resp["block"]["slot"] = block->getSlot();
    
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
  
  if (action == "reinit") {
    if (!reqJson.contains("checkpoint")) {
      resp["error"] = "missing checkpoint field";
      return resp.dump();
    }
    
    auto& cpJson = reqJson["checkpoint"];
    
    if (!cpJson.contains("blockId")) {
      resp["error"] = "missing blockId in checkpoint";
      return resp.dump();
    }
    
    Miner::CheckpointInfo checkpoint;
    checkpoint.blockId = cpJson["blockId"].get<uint64_t>();
    
    if (cpJson.contains("stateData") && cpJson["stateData"].is_array()) {
      checkpoint.stateData = cpJson["stateData"].get<std::vector<uint8_t>>();
    }
    
    auto result = miner_.reinitFromCheckpoint(checkpoint);
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    resp["status"] = "ok";
    resp["message"] = "Reinitialized from checkpoint";
    resp["currentBlockId"] = miner_.getCurrentBlockId();
    
  } else if (action == "isOutOfDate") {
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

void MinerServer::handleSlotLeaderRole() {
  if (miner_.shouldProduceBlock()) {
    log().info << "Attempting to produce block for slot " << miner_.getCurrentSlot();
    
    auto result = miner_.produceBlock();
    if (result) {
      auto block = result.value();
      log().info << "Successfully produced block " << block->getIndex() 
                << " with hash " << block->getHash();
      
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

} // namespace pp
