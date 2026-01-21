#include "BeaconServer.h"
#include "../client/Client.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pp {

BeaconServer::BeaconServer() {
  setLogger("BeaconServer");
  log().info << "BeaconServer initialized";
}

bool BeaconServer::start(const std::string &dataDir) {
  if (isRunning()) {
    log().warning << "BeaconServer is already running";
    return false;
  }

  // Store dataDir for onStart
  dataDir_ = dataDir;

  log().info << "Starting BeaconServer with work directory: " << dataDir;
  log().addFileHandler(dataDir + "/beacon.log", logging::Level::DEBUG);

  // Call base class start which will invoke onStart() then run()
  return Service::start();
}

bool BeaconServer::onStart() {
bool BeaconServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(dataDir_) / "config.json";
  std::string configPathStr = configPath.string();

  // Load configuration (includes port)
  auto configResult = loadConfig(configPathStr);
  if (!configResult) {
    log().error << "Failed to load configuration: "
                << configResult.error().message;
    return false;
  }
  
  // Initialize beacon core
  Beacon::Config beaconConfig;
  beaconConfig.workDir = dataDir_;
  beaconConfig.slotDuration = 1;
  beaconConfig.slotsPerEpoch = 21600;
  
  auto beaconInit = beacon_.init(beaconConfig);
  if (!beaconInit) {
    log().error << "Failed to initialize Beacon: " << beaconInit.error().message;
    return false;
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
  bool serverStarted = fetchServer_.start(fetchServerConfig);

  if (!serverStarted) {
    log().error << "Failed to start FetchServer";
    return false;
  }

  // Connect to other beacon servers
  connectToOtherBeacons();

  return true;
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
      log().debug << "Processing request from queue";
      
      try {
        // Process the request
        std::string response = handleServerRequest(qr.request);
        
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
    } else {
      // No request available, sleep briefly
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  log().info << "Request handler thread stopped";
}
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
    return Error(4, "Failed to parse JSON: " + std::string(e.what()));
  }

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

  log().info << "Configuration loaded from " << configPath;
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Other beacons: " << otherBeaconAddresses_.size();
  return {};
}

std::string BeaconServer::handleServerRequest(const std::string &request) {
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

  if (type == "register") {
    // Server is registering itself
    if (reqJson.contains("address") && reqJson["address"].is_string()) {
      std::string serverAddress = reqJson["address"].get<std::string>();
      registerServer(serverAddress);
      log().info << "Registered server: " << serverAddress;

      nlohmann::json resp;
      resp["status"] = "ok";
      resp["message"] = "Server registered";
      return resp.dump();
    } else {
      nlohmann::json resp;
      resp["error"] = "missing address field";
      return resp.dump();
    }
  } else if (type == "heartbeat") {
    // Server is sending heartbeat
    if (reqJson.contains("address") && reqJson["address"].is_string()) {
      std::string serverAddress = reqJson["address"].get<std::string>();
      registerServer(serverAddress);
      log().debug << "Received heartbeat from: " << serverAddress;

      nlohmann::json resp;
      resp["status"] = "ok";
      return resp.dump();
    } else {
      nlohmann::json resp;
      resp["error"] = "missing address field";
      return resp.dump();
    }
  } else if (type == "query") {
    // Query active servers
    nlohmann::json resp;
    resp["status"] = "ok";
    resp["servers"] = nlohmann::json::array();

    std::vector<std::string> servers = getActiveServers();
    for (const auto &server : servers) {
      resp["servers"].push_back(server);
    }

    return resp.dump();
  } else if (type == "block") {
    return handleBlockRequest(reqJson);
  } else if (type == "checkpoint") {
    return handleCheckpointRequest(reqJson);
  } else if (type == "stakeholder") {
    return handleStakeholderRequest(reqJson);
  } else if (type == "consensus") {
    return handleConsensusRequest(reqJson);
  } else {
    nlohmann::json resp;
    resp["error"] = "unknown request type: " + type;
    return resp.dump();
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

std::string BeaconServer::handleBlockRequest(const nlohmann::json& reqJson) {
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
    auto result = beacon_.getBlock(blockId);
    
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
    
    auto result = beacon_.addBlock(block);
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    resp["status"] = "ok";
    resp["message"] = "Block added successfully";
    
  } else if (action == "current") {
    resp["status"] = "ok";
    resp["currentBlockId"] = beacon_.getCurrentBlockId();
    
  } else {
    resp["error"] = "unknown block action: " + action;
  }
  
  return resp.dump();
}

std::string BeaconServer::handleCheckpointRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "list") {
    auto result = beacon_.getCheckpoints();
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    resp["status"] = "ok";
    resp["checkpoints"] = nlohmann::json::array();
    for (uint64_t checkpointId : result.value()) {
      resp["checkpoints"].push_back(checkpointId);
    }
    
  } else if (action == "current") {
    resp["status"] = "ok";
    resp["currentCheckpointId"] = beacon_.getCurrentCheckpointId();
    
  } else if (action == "evaluate") {
    auto result = beacon_.evaluateCheckpoints();
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    resp["status"] = "ok";
    resp["message"] = "Checkpoints evaluated";
    
  } else {
    resp["error"] = "unknown checkpoint action: " + action;
  }
  
  return resp.dump();
}

std::string BeaconServer::handleStakeholderRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "list") {
    const auto& stakeholders = beacon_.getStakeholders();
    resp["status"] = "ok";
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
      resp["error"] = "missing stakeholder field";
      return resp.dump();
    }
    
    auto& shJson = reqJson["stakeholder"];
    Beacon::Stakeholder sh;
    
    if (!shJson.contains("id") || !shJson.contains("stake")) {
      resp["error"] = "missing required stakeholder fields (id, stake)";
      return resp.dump();
    }
    
    sh.id = shJson["id"].get<std::string>();
    sh.stake = shJson["stake"].get<uint64_t>();
    
    if (shJson.contains("address")) {
      sh.endpoint.address = shJson["address"].get<std::string>();
    }
    if (shJson.contains("port")) {
      sh.endpoint.port = shJson["port"].get<uint16_t>();
    }
    
    beacon_.addStakeholder(sh);
    resp["status"] = "ok";
    resp["message"] = "Stakeholder added";
    
  } else if (action == "remove") {
    if (!reqJson.contains("id")) {
      resp["error"] = "missing id field";
      return resp.dump();
    }
    
    std::string id = reqJson["id"].get<std::string>();
    beacon_.removeStakeholder(id);
    resp["status"] = "ok";
    resp["message"] = "Stakeholder removed";
    
  } else if (action == "updateStake") {
    if (!reqJson.contains("id") || !reqJson.contains("stake")) {
      resp["error"] = "missing id or stake field";
      return resp.dump();
    }
    
    std::string id = reqJson["id"].get<std::string>();
    uint64_t stake = reqJson["stake"].get<uint64_t>();
    
    beacon_.updateStake(id, stake);
    resp["status"] = "ok";
    resp["message"] = "Stake updated";
    
  } else {
    resp["error"] = "unknown stakeholder action: " + action;
  }
  
  return resp.dump();
}

std::string BeaconServer::handleConsensusRequest(const nlohmann::json& reqJson) {
  nlohmann::json resp;
  
  if (!reqJson.contains("action")) {
    resp["error"] = "missing action field";
    return resp.dump();
  }
  
  std::string action = reqJson["action"].get<std::string>();
  
  if (action == "currentSlot") {
    resp["status"] = "ok";
    resp["currentSlot"] = beacon_.getCurrentSlot();
    
  } else if (action == "currentEpoch") {
    resp["status"] = "ok";
    resp["currentEpoch"] = beacon_.getCurrentEpoch();
    
  } else if (action == "slotLeader") {
    if (!reqJson.contains("slot")) {
      resp["error"] = "missing slot field";
      return resp.dump();
    }
    
    uint64_t slot = reqJson["slot"].get<uint64_t>();
    auto result = beacon_.getSlotLeader(slot);
    
    if (!result) {
      resp["error"] = result.error().message;
      return resp.dump();
    }
    
    resp["status"] = "ok";
    resp["slotLeader"] = result.value();
    
  } else {
    resp["error"] = "unknown consensus action: " + action;
  }
  
  return resp.dump();
}

} // namespace pp
