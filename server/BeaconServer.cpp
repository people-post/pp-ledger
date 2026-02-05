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

// ============ InitFileConfig methods ============

nlohmann::json BeaconServer::InitFileConfig::ltsToJson() {
  nlohmann::json j;
  j["slotDuration"] = slotDuration;
  j["slotsPerEpoch"] = slotsPerEpoch;
  j["maxPendingTransactions"] = maxPendingTransactions;
  j["maxTransactionsPerBlock"] = maxTransactionsPerBlock;
  j["minFeePerTransaction"] = minFeePerTransaction;
  return j;
}

BeaconServer::Roe<void> BeaconServer::InitFileConfig::ltsFromJson(const nlohmann::json& jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
    }

    // Load and validate slotDuration
    if (jd.contains("slotDuration")) {
      if (!jd["slotDuration"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'slotDuration' must be a positive number");
      }
      slotDuration = jd["slotDuration"].get<uint64_t>();
      if (slotDuration == 0) {
        return Error(E_CONFIG, "Field 'slotDuration' must be greater than 0");
      }
    } else {
      slotDuration = DEFAULT_SLOT_DURATION;
    }

    // Load and validate slotsPerEpoch
    if (jd.contains("slotsPerEpoch")) {
      if (!jd["slotsPerEpoch"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'slotsPerEpoch' must be a positive number");
      }
      slotsPerEpoch = jd["slotsPerEpoch"].get<uint64_t>();
      if (slotsPerEpoch == 0) {
        return Error(E_CONFIG, "Field 'slotsPerEpoch' must be greater than 0");
      }
    } else {
      slotsPerEpoch = DEFAULT_SLOTS_PER_EPOCH;
    }

    // Load and validate maxPendingTransactions
    if (jd.contains("maxPendingTransactions")) {
      if (!jd["maxPendingTransactions"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'maxPendingTransactions' must be a positive number");
      }
      maxPendingTransactions = jd["maxPendingTransactions"].get<uint64_t>();
      if (maxPendingTransactions == 0) {
        return Error(E_CONFIG, "Field 'maxPendingTransactions' must be greater than 0");
      }
    } else {
      maxPendingTransactions = DEFAULT_MAX_PENDING_TRANSACTIONS;
    }

    // Load and validate maxTransactionsPerBlock
    if (jd.contains("maxTransactionsPerBlock")) {
      if (!jd["maxTransactionsPerBlock"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'maxTransactionsPerBlock' must be a positive number");
      }
      maxTransactionsPerBlock = jd["maxTransactionsPerBlock"].get<uint64_t>();
      if (maxTransactionsPerBlock == 0) {
        return Error(E_CONFIG, "Field 'maxTransactionsPerBlock' must be greater than 0");
      }
    } else {
      maxTransactionsPerBlock = DEFAULT_MAX_TRANSACTIONS_PER_BLOCK;
    }

    // Load and validate minFeePerTransaction
    if (jd.contains("minFeePerTransaction")) {
      if (!jd["minFeePerTransaction"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'minFeePerTransaction' must be a positive number");
      }
      minFeePerTransaction = jd["minFeePerTransaction"].get<uint64_t>();
    } else {
      minFeePerTransaction = DEFAULT_MIN_FEE_PER_TRANSACTION;
    }

    return {};
  } catch (const std::exception& e) {
    return Error(E_CONFIG, "Failed to parse init configuration: " + std::string(e.what()));
  }
}

// ============ RunFileConfig methods ============

nlohmann::json BeaconServer::RunFileConfig::ltsToJson() {
  nlohmann::json j;
  j["host"] = host;
  j["port"] = port;
  j["beacons"] = beacons;
  j["checkpointSize"] = checkpointSize;
  j["checkpointAge"] = checkpointAge;
  return j;
}

BeaconServer::Roe<void> BeaconServer::RunFileConfig::ltsFromJson(const nlohmann::json& jd) {
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

    // Load and validate beacons array
    beacons.clear();
    if (jd.contains("beacons")) {
      if (!jd["beacons"].is_array()) {
        return Error(E_CONFIG, "Field 'beacons' must be an array");
      }
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
    }

    // Load and validate checkpointSize
    if (jd.contains("checkpointSize")) {
      if (!jd["checkpointSize"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'checkpointSize' must be a positive number");
      }
      checkpointSize = jd["checkpointSize"].get<uint64_t>();
      if (checkpointSize == 0) {
        return Error(E_CONFIG, "Field 'checkpointSize' must be greater than 0");
      }
    } else {
      checkpointSize = DEFAULT_CHECKPOINT_SIZE;
    }

    // Load and validate checkpointAge
    if (jd.contains("checkpointAge")) {
      if (!jd["checkpointAge"].is_number_unsigned()) {
        return Error(E_CONFIG, "Field 'checkpointAge' must be a positive number");
      }
      checkpointAge = jd["checkpointAge"].get<uint64_t>();
      if (checkpointAge == 0) {
        return Error(E_CONFIG, "Field 'checkpointAge' must be greater than 0");
      }
    } else {
      checkpointAge = DEFAULT_CHECKPOINT_AGE;
    }

    return {};
  } catch (const std::exception& e) {
    return Error(E_CONFIG, "Failed to parse run configuration: " + std::string(e.what()));
  }
}

// ============ BeaconServer methods ============


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
  
  // Create or load FILE_INIT_CONFIG using InitFileConfig
  InitFileConfig initFileConfig;
  
  if (!std::filesystem::exists(initConfigPath)) {
    log().info << "Creating " << FILE_INIT_CONFIG << " with default parameters";
    
    // Use default values from InitFileConfig struct
    nlohmann::json defaultConfig = initFileConfig.ltsToJson();
    
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
  
  // Use InitFileConfig to parse configuration
  auto parseResult = initFileConfig.ltsFromJson(config);
  if (!parseResult) {
    return Error("Failed to parse init config file: " + parseResult.error().message);
  }
  
  log().info << "Configuration:";
  log().info << "  Slot duration: " << initFileConfig.slotDuration << " seconds";
  log().info << "  Slots per epoch: " << initFileConfig.slotsPerEpoch;
  log().info << "  Max pending transactions: " << initFileConfig.maxPendingTransactions;
  log().info << "  Max transactions per block: " << initFileConfig.maxTransactionsPerBlock;
  
  // Prepare init configuration
  Beacon::InitConfig initConfig;
  initConfig.workDir = workDir + "/" + DIR_DATA;
  initConfig.chain.slotDuration = initFileConfig.slotDuration;
  initConfig.chain.slotsPerEpoch = initFileConfig.slotsPerEpoch;
  initConfig.chain.maxPendingTransactions = initFileConfig.maxPendingTransactions;
  initConfig.chain.maxTransactionsPerBlock = initFileConfig.maxTransactionsPerBlock;
  
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
  // Store dataDir for onStart
  workDir_ = workDir;

  log().info << "Starting with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + FILE_LOG, logging::Level::DEBUG);

  // Call base class start which will invoke onStart() then runLoop()
  return Service::start();
}

Service::Roe<void> BeaconServer::onStart() {
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
      return Service::Error(-2, "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << defaultConfig.dump(2) << std::endl;
    configFile.close();
    
    log().info << "Created " << FILE_CONFIG << " at: " << configPathStr;
  } else {
    // Load existing configuration
    auto jsonResult = utl::loadJsonFile(configPathStr);
    if (!jsonResult) {
      return Service::Error(-3, "Failed to load config file: " + jsonResult.error().message);
    }
    
    nlohmann::json config = jsonResult.value();
    auto parseResult = runFileConfig.ltsFromJson(config);
    if (!parseResult) {
      return Service::Error(E_CONFIG, "Failed to parse config file: " + parseResult.error().message);
    }
  }

  // Apply configuration from RunFileConfig
  config_.network.endpoint.address = runFileConfig.host;
  config_.network.endpoint.port = runFileConfig.port;
  config_.checkpoint.minSizeBytes = runFileConfig.checkpointSize;
  config_.checkpoint.ageSeconds = runFileConfig.checkpointAge;
  
  // Store beacon addresses
  otherBeaconAddresses_ = runFileConfig.beacons;
  
  log().info << "Configuration loaded";
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Other beacons: " << otherBeaconAddresses_.size();
  log().info << "  Checkpoint size: " << (config_.checkpoint.minSizeBytes / (1024*1024)) << " MB";
  log().info << "  Checkpoint age: " << (config_.checkpoint.ageSeconds / (24*3600)) << " days";
  
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

void BeaconServer::runLoop() {
  log().info << "Request handler thread started";

  while (!isStopSet()) {
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
  log().debug << "Handling request: " << request.type;

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
  // Get current timestamp
  int64_t now = std::chrono::system_clock::now()
                    .time_since_epoch()
                    .count();

  // Update or add server
  activeServers_[serverAddress] = now;
  log().debug << "Updated server record: " << serverAddress;
}

std::vector<std::string> BeaconServer::getActiveServers() const {
  std::vector<std::string> servers;
  for (const auto &pair : activeServers_) {
    servers.push_back(pair.first);
  }
  return servers;
}

size_t BeaconServer::getActiveServerCount() const {
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

  return buildStateResponse().dump();
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
      shJson["stake"] = sh.stake;
      resp["stakeholders"].push_back(shJson);
    }
    
    resp["totalStake"] = beacon_.getTotalStake();
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
    resp = buildStateResponse();
  } else {
    return Error(E_REQUEST, "Unknown state action: " + action);
  }

  return resp.dump();
}

nlohmann::json BeaconServer::buildStateResponse() const {
  int64_t currentTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  Client::BeaconState state;
  state.currentTimestamp = currentTimestamp;
  state.lastCheckpointId = beacon_.getLastCheckpointId();
  state.checkpointId = beacon_.getCurrentCheckpointId();
  state.nextBlockId = beacon_.getNextBlockId();
  state.currentSlot = beacon_.getCurrentSlot();
  state.currentEpoch = beacon_.getCurrentEpoch();
  state.nStakeholders = beacon_.getStakeholders().size();
  
  return state.ltsToJson();
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
