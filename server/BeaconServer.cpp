#include "BeaconServer.h"
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

BeaconServer::BeaconServer() {
  redirectLogger("BeaconServer");
  beacon_.redirectLogger(log().getFullName() + ".Beacon");
  fetchServer_.redirectLogger(log().getFullName() + ".FetchServer");
}

BeaconServer::Roe<void> BeaconServer::init(const std::string& workDir) {
  log().info << "Initializing new beacon with work directory: " << workDir;
  
  std::filesystem::path workDirPath(workDir);
  std::filesystem::path initConfigPath = workDirPath / FILE_INIT_CONFIG;
  std::filesystem::path signaturePath = workDirPath / FILE_SIGNATURE;
  
  if (std::filesystem::exists(workDirPath)) {
    if (!std::filesystem::exists(signaturePath)) {
      return Error("Work directory not recognized, please remove it manually and try again");
    }
  } else {
    // Create work directory if it doesn't exist (to write config file)
    std::filesystem::create_directories(workDirPath);
    // Create signature file to mark work directory as recognized
    auto result = utl::writeToNewFile(signaturePath.string(), "");
    if (!result) {
      return Error("Failed to create signature file: " + result.error().message);
    }
    log().info << "Created work directory: " << workDir;
  }
  
  // Create or load FILE_INIT_CONFIG
  if (!std::filesystem::exists(initConfigPath)) {
    log().info << "Creating " << FILE_INIT_CONFIG << " with default parameters";
    
    nlohmann::json defaultConfig;
    defaultConfig["slotDuration"] = DEFAULT_SLOT_DURATION;
    defaultConfig["slotsPerEpoch"] = DEFAULT_SLOTS_PER_EPOCH;
    defaultConfig["maxPendingTransactions"] = DEFAULT_MAX_PENDING_TRANSACTIONS;
    defaultConfig["maxTransactionsPerBlock"] = DEFAULT_MAX_TRANSACTIONS_PER_BLOCK;
    
    auto result = utl::writeToNewFile(initConfigPath.string(), defaultConfig.dump(2));
    if (!result) {
      return Error("Failed to create " + std::string(FILE_INIT_CONFIG) + ": " + result.error().message);
    }
    
    log().info << "Created: " << initConfigPath.string();
  } else {
    log().info << "Found existing " << FILE_INIT_CONFIG;
  }
  
  // Load configuration from FILE_INIT_CONFIG
  log().info << "Loading configuration from: " << initConfigPath.string();
  
  auto jsonResult = utl::loadJsonFile(initConfigPath.string());
  if (!jsonResult) {
    return Error("Failed to load init config file: " + jsonResult.error().message);
  }
  
  nlohmann::json config = jsonResult.value();
  
  // Extract configuration with defaults
  uint64_t slotDuration = config.value("slotDuration", DEFAULT_SLOT_DURATION);
  uint64_t slotsPerEpoch = config.value("slotsPerEpoch", DEFAULT_SLOTS_PER_EPOCH);
  uint64_t maxPendingTransactions = config.value("maxPendingTransactions", DEFAULT_MAX_PENDING_TRANSACTIONS);
  uint64_t maxTransactionsPerBlock = config.value("maxTransactionsPerBlock", DEFAULT_MAX_TRANSACTIONS_PER_BLOCK);
  
  log().info << "Configuration:";
  log().info << "  Slot duration: " << slotDuration << " seconds";
  log().info << "  Slots per epoch: " << slotsPerEpoch;
  
  // Prepare init configuration
  Beacon::InitConfig initConfig;
  initConfig.workDir = workDir + "/" + DIR_DATA;
  initConfig.chain.slotDuration = slotDuration;
  initConfig.chain.slotsPerEpoch = slotsPerEpoch;
  initConfig.chain.maxPendingTransactions = maxPendingTransactions;
  initConfig.chain.maxTransactionsPerBlock = maxTransactionsPerBlock;
  
  // Call the existing initFromWorkDir method
  auto result = initFromWorkDir(initConfig);
  if (!result) {
    return result;
  }
  
  log().info << "Beacon initialized successfully";
  return {};
}

BeaconServer::Roe<void> BeaconServer::initFromWorkDir(const Beacon::InitConfig& config) {
  log().info << "Initializing BeaconServer";
  
  // Clean up work directory if it exists
  if (std::filesystem::exists(config.workDir)) {
    log().info << "  Removing existing work directory: " << config.workDir;
    std::error_code ec;
    std::filesystem::remove_all(config.workDir, ec);
    if (ec) {
      return Error("Failed to remove existing work directory: " + ec.message());
    }
  }
  
  // Initialize beacon (which will create fresh directory)
  auto result = beacon_.init(config);
  if (!result) {
    return Error("Failed to initialize beacon: " + result.error().message);
  }
  
  log().info << "BeaconServer initialization complete";
  return {};
}

Service::Roe<void> BeaconServer::start(const std::string &workDir) {
  if (isRunning()) {
    return Service::Error(-1, "BeaconServer is already running");
  }

  // Store dataDir for onStart
  workDir_ = workDir;

  log().info << "Starting with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + FILE_LOG, logging::Level::DEBUG);

  // Call base class start which will invoke onStart() then run()
  return Service::start();
}

Service::Roe<void> BeaconServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(workDir_) / FILE_CONFIG;
  std::string configPathStr = configPath.string();

  // Create default FILE_CONFIG if it doesn't exist
  if (!std::filesystem::exists(configPath)) {
    log().info << "No " << FILE_CONFIG << " found, creating with default values";
    
    nlohmann::json defaultConfig;
    defaultConfig["host"] = Client::DEFAULT_HOST;
    defaultConfig["port"] = Client::DEFAULT_BEACON_PORT;
    defaultConfig["beacons"] = nlohmann::json::array();
    defaultConfig["checkpointSize"] = DEFAULT_CHECKPOINT_SIZE; // 1 GB
    defaultConfig["checkpointAge"] = DEFAULT_CHECKPOINT_AGE; // 1 year
    
    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(-2, "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << defaultConfig.dump(2) << std::endl;
    configFile.close();
    
    log().info << "Created " << FILE_CONFIG << " at: " << configPathStr;
  }

  // Load configuration (includes port)
  auto configResult = loadConfig(configPathStr);
  if (!configResult) {
    return Service::Error(-3, "Failed to load configuration: " + configResult.error().message);
  }
  
  // Initialize beacon core with mount config
  Beacon::MountConfig mountConfig;
  mountConfig.workDir = workDir_ + "/" + DIR_DATA;
  // Use checkpoint config from loaded config (or defaults)
  mountConfig.checkpoint = config_.checkpoint;
  
  auto beaconMount = beacon_.mount(mountConfig);
  if (!beaconMount) {
    return Service::Error(-4, "Failed to mount Beacon: " + beaconMount.error().message);
  }
  
  log().info << "Beacon core initialized";

  // Start FetchServer with handler that enqueues requests
  network::FetchServer::Config fetchServerConfig;
  fetchServerConfig.endpoint = config_.network.endpoint;
  fetchServerConfig.handler = [this](const std::string &request, std::shared_ptr<network::TcpConnection> conn) {
    QueuedRequest qr;
    qr.request = request;
    qr.connection = conn;
    requestQueue_.push(std::move(qr));
    log().debug << "Request enqueued (queue size: " << requestQueue_.size() << ")";
  };
  auto serverStarted = fetchServer_.start(fetchServerConfig);
  if (!serverStarted) {
    return Service::Error(-5, "Failed to start FetchServer: " + serverStarted.error().message);
  }

  // Connect to other beacon servers
  connectToOtherBeacons();
  return {};
}

void BeaconServer::onStop() {
  fetchServer_.stop();
  log().info << "BeaconServer resources cleaned up";
}

void BeaconServer::run() {
  log().info << "Request handler thread started";

  while (isRunning()) {
    QueuedRequest qr;
    
    // Poll for a request from the queue
    if (requestQueue_.poll(qr)) {
      processQueuedRequest(qr);
    } else {
      // No request available, sleep briefly
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  log().info << "Request handler thread stopped";
}

void BeaconServer::processQueuedRequest(QueuedRequest& qr) {
  log().debug << "Processing request from queue";
  try {
    // Process the request
    std::string response = handleRequest(qr.request);
    
    // Send response back
    auto sendResult = qr.connection->send(response);
    if (!sendResult) {
      log().error << "Failed to send response: " << sendResult.error().message;
    } else {
      log().debug << "Response sent (" << response.size() << " bytes)";
    }
    
    // Close the connection
    qr.connection->close();
    
  } catch (const std::exception& e) {
    log().error << "Exception processing request: " << e.what();
    // Try to close connection even on error
    try {
      qr.connection->close();
    } catch (...) {
      // Ignore close errors
    }
  }
}

BeaconServer::Roe<void> BeaconServer::loadConfig(const std::string &configPath) {
  // Use the utility function for loading JSON
  auto jsonResult = utl::loadJsonFile(configPath);
  if (jsonResult.isError()) {
    // Convert pp::Error to BeaconServer::Error
    return Error(jsonResult.error().code, jsonResult.error().message);
  }

  nlohmann::json config = jsonResult.value();

  // Load host (optional, default: "localhost")
  if (config.contains("host") && config["host"].is_string()) {
    config_.network.endpoint.address = config["host"].get<std::string>();
  } else {
    config_.network.endpoint.address = Client::DEFAULT_HOST;
  }

  // Load port (optional, default: 8517)
  if (config.contains("port") && config["port"].is_number()) {
    if (config["port"].is_number_integer()) {
      config_.network.endpoint.port = config["port"].get<uint16_t>();
    } else {
      return Error(3, "Configuration file 'port' field is not an integer");
    }
  } else {
    config_.network.endpoint.port = Client::DEFAULT_BEACON_PORT;
  }

  // Load other beacon addresses (optional, can be empty)
  otherBeaconAddresses_.clear();
  if (config.contains("beacons") && config["beacons"].is_array()) {
    for (const auto &beaconAddr : config["beacons"]) {
      if (beaconAddr.is_string()) {
        std::string addr = beaconAddr.get<std::string>();
        otherBeaconAddresses_.push_back(addr);
        log().info << "Found beacon address in config: " << addr;
      }
    }
  }

  // Load checkpoint configuration (optional)
  if (config.contains("checkpointSize") && config["checkpointSize"].is_number()) {
    config_.checkpoint.minSizeBytes = config["checkpointSize"].get<uint64_t>();
  }
  if (config.contains("checkpointAge") && config["checkpointAge"].is_number()) {
    config_.checkpoint.ageSeconds = config["checkpointAge"].get<uint64_t>();
  }

  log().info << "Configuration loaded from " << configPath;
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Other beacons: " << otherBeaconAddresses_.size();
  log().info << "  Checkpoint size: " << (config_.checkpoint.minSizeBytes / (1024*1024)) << " MB";
  log().info << "  Checkpoint age: " << (config_.checkpoint.ageSeconds / (24*3600)) << " days";
  return {};
}

std::string BeaconServer::handleRequest(const std::string &request) {
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

BeaconServer::Roe<std::string> BeaconServer::handleRequest(const Client::Request &request) {
  switch (request.type) {
  case Client::T_REQ_BLOCK_GET:
    return handleBlockGetRequest(request);
  case Client::T_REQ_BLOCK_ADD:
    return handleBlockAddRequest(request);
  case Client::T_REQ_JSON:
    return handleJsonRequest(request.payload);
  default:
    return Error(E_REQUEST, "Unknown request type: " + std::to_string(request.type));
  }
}

BeaconServer::Roe<std::string> BeaconServer::handleBlockGetRequest(const Client::Request &request) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    return Error(E_REQUEST, "Invalid block get payload: " + request.payload);
  }

  uint64_t blockId = idResult.value();
  auto result = beacon_.getBlock(blockId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get block: " + result.error().message);
  }

  return result.value().ltsToString();
}

BeaconServer::Roe<std::string> BeaconServer::handleBlockAddRequest(const Client::Request &request) {
  Ledger::ChainNode block;
  if (!block.ltsFromString(request.payload)) {
    return Error(E_REQUEST, "Failed to deserialize block: " + request.payload);
  }
  block.hash = beacon_.calculateHash(block.block);
  auto result = beacon_.addBlock(block);
  if (!result) {
    return Error(E_REQUEST, "Failed to add block: " + result.error().message);
  }
  nlohmann::json resp;
  resp["message"] = "Block added";
  return resp.dump();
}

BeaconServer::Roe<std::string> BeaconServer::handleJsonRequest(const std::string &payload) {
  auto jsonResult = utl::parseJsonRequest(payload);
  if (jsonResult.isError()) {
    return Error(E_REQUEST, "Failed to parse request JSON: " + jsonResult.error().message);
  }
  return handleJsonRequest(jsonResult.value());
}

BeaconServer::Roe<std::string> BeaconServer::handleJsonRequest(const nlohmann::json &reqJson) {
  if (!reqJson.contains("type")) {
    return Error(E_REQUEST, "Missing type field in request JSON");
  }
  std::string type = reqJson["type"].get<std::string>();

  if (type == "register") {
    return handleRegisterRequest(reqJson);
  } else if (type == "heartbeat") {
    return handleHeartbeatRequest(reqJson);
  } else if (type == "query") {
    return handleQueryRequest(reqJson);
  } else if (type == "checkpoint") {
    return handleCheckpointRequest(reqJson);
  } else if (type == "stakeholder") {
    return handleStakeholderRequest(reqJson);
  } else if (type == "consensus") {
    return handleConsensusRequest(reqJson);
  } else if (type == "state") {
    return handleStateRequest(reqJson);
  } else {
    return Error(E_REQUEST, "Unknown request type: " + type);
  }
}

void BeaconServer::registerServer(const std::string &serverAddress) {
  std::lock_guard<std::mutex> lock(serversMutex_);

  // Get current timestamp
  int64_t now = std::chrono::system_clock::now()
                    .time_since_epoch()
                    .count();

  // Update or add server
  activeServers_[serverAddress] = now;
  log().debug << "Updated server record: " << serverAddress;
}

std::vector<std::string> BeaconServer::getActiveServers() const {
  std::lock_guard<std::mutex> lock(serversMutex_);

  std::vector<std::string> servers;
  for (const auto &pair : activeServers_) {
    servers.push_back(pair.first);
  }
  return servers;
}

size_t BeaconServer::getActiveServerCount() const {
  std::lock_guard<std::mutex> lock(serversMutex_);
  return activeServers_.size();
}

void BeaconServer::connectToOtherBeacons() {
  if (otherBeaconAddresses_.empty()) {
    log().info << "No other beacon addresses configured";
    return;
  }

  log().info << "Connecting to " << otherBeaconAddresses_.size()
             << " other beacon servers";

  // For now, just log the addresses
  // In a full implementation, we might want to establish connections
  // to share server lists between beacons
  for (const auto &beaconAddr : otherBeaconAddresses_) {
    log().info << "  Beacon: " << beaconAddr;
  }
}

BeaconServer::Roe<std::string> BeaconServer::handleRegisterRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("address") || !reqJson["address"].is_string()) {
    return Error(E_REQUEST, "Missing address field in request JSON");
  }
  std::string serverAddress = reqJson["address"].get<std::string>();
  registerServer(serverAddress);
  nlohmann::json resp;
  resp["message"] = "Server registered";
  return resp.dump();
}

BeaconServer::Roe<std::string> BeaconServer::handleHeartbeatRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("address") || !reqJson["address"].is_string()) {
    return Error(E_REQUEST, "Missing address field in request JSON");
  }
  std::string serverAddress = reqJson["address"].get<std::string>();
  registerServer(serverAddress);

  nlohmann::json resp;
  resp["message"] = "Heartbeat received";
  return resp.dump();
}

BeaconServer::Roe<std::string> BeaconServer::handleQueryRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  resp["servers"] = nlohmann::json::array();

  std::vector<std::string> servers = getActiveServers();
  for (const auto &server : servers) {
    resp["servers"].push_back(server);
  }
  
  return resp.dump();
}

BeaconServer::Roe<std::string> BeaconServer::handleCheckpointRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("action")) {
    return Error(E_REQUEST, "Missing action field in request JSON");
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  nlohmann::json resp;
  
  if (action == "list") {
    auto result = beacon_.getCheckpoints();
    if (!result) {
      return Error(E_REQUEST, "Failed to get checkpoints: " + result.error().message);
    }
    
    resp["checkpoints"] = nlohmann::json::array();
    for (uint64_t checkpointId : result.value()) {
      resp["checkpoints"].push_back(checkpointId);
    }
    
  } else if (action == "current") {
    resp["currentCheckpointId"] = beacon_.getCurrentCheckpointId();
    
  } else if (action == "evaluate") {
    auto result = beacon_.evaluateCheckpoints();
    if (!result) {
      return Error(E_REQUEST, "Failed to evaluate checkpoints: " + result.error().message);
    }
    
    resp["message"] = "Checkpoints evaluated";
    
  } else {
    return Error(E_REQUEST, "Unknown checkpoint action: " + action);
  }
  
  return resp.dump();
}

BeaconServer::Roe<std::string> BeaconServer::handleStakeholderRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("action")) {
    return Error(E_REQUEST, "Missing action field in request JSON");
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  nlohmann::json resp;
  if (action == "list") {
    const auto& stakeholders = beacon_.getStakeholders();
    resp["stakeholders"] = nlohmann::json::array();
    
    for (const auto& sh : stakeholders) {
      nlohmann::json shJson;
      shJson["id"] = sh.id;
      shJson["address"] = sh.endpoint.address;
      shJson["port"] = sh.endpoint.port;
      shJson["stake"] = sh.stake;
      resp["stakeholders"].push_back(shJson);
    }
    
    resp["totalStake"] = beacon_.getTotalStake();
    
  } else if (action == "add") {
    if (!reqJson.contains("stakeholder")) {
      return Error(E_REQUEST, "Missing stakeholder field in request JSON");
    }
    
    auto& shJson = reqJson["stakeholder"];
    Beacon::Stakeholder sh;
    
    if (!shJson.contains("id") || !shJson.contains("stake")) {
      return Error(E_REQUEST, "Missing required stakeholder fields (id, stake)");
    }
    
    sh.id = shJson["id"].get<uint64_t>();
    sh.stake = shJson["stake"].get<uint64_t>();
    
    if (shJson.contains("address")) {
      sh.endpoint.address = shJson["address"].get<std::string>();
    }
    if (shJson.contains("port")) {
      sh.endpoint.port = shJson["port"].get<uint16_t>();
    }
    
    beacon_.addStakeholder(sh);
    resp["message"] = "Stakeholder added";
    
  } else if (action == "remove") {
    if (!reqJson.contains("id")) {
      return Error(E_REQUEST, "Missing id field in request JSON");
    }
    
    uint64_t id = reqJson["id"].get<uint64_t>();
    beacon_.removeStakeholder(id);
    resp["message"] = "Stakeholder removed";
    
  } else if (action == "updateStake") {
    if (!reqJson.contains("id") || !reqJson.contains("stake")) {
      return Error(E_REQUEST, "Missing id or stake field in request JSON");
    }
    
    uint64_t id = reqJson["id"].get<uint64_t>();
    uint64_t stake = reqJson["stake"].get<uint64_t>();
    
    beacon_.updateStake(id, stake);
    resp["message"] = "Stake updated";
    
  } else {
    return Error(E_REQUEST, "Unknown stakeholder action: " + action);
  }
  
  return resp.dump();
}

BeaconServer::Roe<std::string> BeaconServer::handleConsensusRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("action")) {
    return Error(E_REQUEST, "Missing action field in request JSON");
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  nlohmann::json resp;
  if (action == "currentSlot") {
    resp["currentSlot"] = beacon_.getCurrentSlot();
  } else if (action == "currentEpoch") {
    resp["currentEpoch"] = beacon_.getCurrentEpoch();
  } else if (action == "slotLeader") {
    if (!reqJson.contains("slot")) {
      return Error(E_REQUEST, "Missing slot field in request JSON");
    }
    
    uint64_t slot = reqJson["slot"].get<uint64_t>();
    auto result = beacon_.getSlotLeader(slot);

    if (!result) {
      return Error(E_REQUEST, "Failed to get slot leader: " + result.error().message);
    }

    resp["slotLeader"] = result.value();
  } else {
    return Error(E_REQUEST, "Unknown consensus action: " + action);
  }

  return resp.dump();
}

BeaconServer::Roe<std::string> BeaconServer::handleStateRequest(const nlohmann::json& reqJson) {
  if (!reqJson.contains("action")) {
    return Error(E_REQUEST, "Missing action field in request JSON");
  }

  std::string action = reqJson["action"].get<std::string>();

  nlohmann::json resp;
  if (action == "current") {
    int64_t currentTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    resp["lastCheckpointId"] = beacon_.getLastCheckpointId();
    resp["currentCheckpointId"] = beacon_.getCurrentCheckpointId();
    resp["nextBlockId"] = beacon_.getNextBlockId();
    resp["currentSlot"] = beacon_.getCurrentSlot();
    resp["currentEpoch"] = beacon_.getCurrentEpoch();
    resp["currentTimestamp"] = currentTimestamp;
    resp["stakeholders"] = nlohmann::json::array();

    for (const auto& sh : beacon_.getStakeholders()) {
      nlohmann::json shJson;
      shJson["id"] = sh.id;
      shJson["address"] = sh.endpoint.address;
      shJson["port"] = sh.endpoint.port;
      shJson["stake"] = sh.stake;
      resp["stakeholders"].push_back(shJson);
    }
  } else {
    return Error(E_REQUEST, "Unknown state action: " + action);
  }

  return resp.dump();
}

std::string BeaconServer::binaryResponseOk(const std::string& payload) const {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = 0;
  resp.payload = payload;
  return utl::binaryPack(resp);
}

std::string BeaconServer::binaryResponseError(uint16_t errorCode, const std::string& message) const {
  Client::Response resp;
  resp.version = Client::Response::VERSION;
  resp.errorCode = errorCode;
  resp.payload = message;
  return utl::binaryPack(resp);
}

} // namespace pp
