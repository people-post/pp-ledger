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
  client_.redirectLogger(log().getFullName() + ".Client");
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

nlohmann::json BeaconServer::PrimaryBeaconConfig::ltsToJson() {
  nlohmann::json j;
  j["trustedBeacons"] = trustedBeacons;
  j["checkpointSize"] = checkpointSize;
  j["checkpointAge"] = checkpointAge;
  return j;
}

BeaconServer::Roe<void> BeaconServer::PrimaryBeaconConfig::ltsFromJson(const nlohmann::json& jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
    }

    // Load and validate trustedBeacons
    if (jd.contains("trustedBeacons")) {
      if (!jd["trustedBeacons"].is_array()) {
        return Error(E_CONFIG, "Field 'trustedBeacons' must be an array");
      }
    }
    for (const auto& beacon : jd["trustedBeacons"]) {
      if (!beacon.is_string()) {
        return Error(E_CONFIG, "All elements in 'trustedBeacons' array must be strings");
      }
      trustedBeacons.push_back(beacon.get<std::string>());
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
    return Error(E_CONFIG, "Failed to parse primary beacon configuration: " + std::string(e.what()));
  }
}

nlohmann::json BeaconServer::InputBeaconConfig::ltsToJson() {
  nlohmann::json j;
  j["primaryBeacon"] = primaryBeacon;
  return j;
}

BeaconServer::Roe<void> BeaconServer::InputBeaconConfig::ltsFromJson(const nlohmann::json& jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
    }

    // Load and validate primaryBeacon
    if (jd.contains("primaryBeacon")) {
      if (!jd["primaryBeacon"].is_string()) {
        return Error(E_CONFIG, "Field 'primaryBeacon' must be a string");
      }
      primaryBeacon = jd["primaryBeacon"].get<std::string>();
    } else {
      return Error(E_CONFIG, "Field 'primaryBeacon' is required");
    }

    return {};
  } catch (const std::exception& e) {
    return Error(E_CONFIG, "Failed to parse input beacon configuration: " + std::string(e.what()));
  }
}

nlohmann::json BeaconServer::OutputBeaconConfig::ltsToJson() {
  nlohmann::json j;
  j["primaryBeacon"] = primaryBeacon;
  return j;
}

BeaconServer::Roe<void> BeaconServer::OutputBeaconConfig::ltsFromJson(const nlohmann::json& jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
    }

    // Load and validate primaryBeacon
    if (jd.contains("primaryBeacon")) {
      if (!jd["primaryBeacon"].is_string()) {
        return Error(E_CONFIG, "Field 'primaryBeacon' must be a string");
      }
      primaryBeacon = jd["primaryBeacon"].get<std::string>();
    } else {
      return Error(E_CONFIG, "Field 'primaryBeacon' is required");
    }

    return {};
  } catch (const std::exception& e) {
    return Error(E_CONFIG, "Failed to parse output beacon configuration: " + std::string(e.what()));
  }
}

nlohmann::json BeaconServer::RunFileConfig::ltsToJson() {
  nlohmann::json j;
  j["host"] = host;
  j["port"] = port;
  switch (mode) {
  case Mode::Primary:
    j["mode"] = "primary";
    break;
  case Mode::Input:
    j["mode"] = "input";
    break;
  case Mode::Output:
    j["mode"] = "output";
    break;
  }
  j["primary"] = primary.ltsToJson();
  j["input"] = input.ltsToJson();
  j["output"] = output.ltsToJson();
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

    // Load and validate mode
    if (jd.contains("mode")) {
      if (!jd["mode"].is_string()) {
        return Error(E_CONFIG, "Field 'mode' must be a string");
      }
      std::string modeName = jd["mode"].get<std::string>();
      if (modeName == "primary") {
        mode = Mode::Primary;
      } else if (modeName == "input") {
        mode = Mode::Input;
      } else if (modeName == "output") {
        mode = Mode::Output;
      } else {
        return Error(E_CONFIG, "Invalid mode: " + modeName);
      }
    } else {
      return Error(E_CONFIG, "Field 'mode' is required");
    }

    // Load and validate primary beacon configuration
    if (mode == Mode::Primary) {
      if (jd.contains("primary")) {
        if (!jd["primary"].is_object()) {
          return Error(E_CONFIG, "Field 'primary' must be an object");
        }
        auto primaryConfig = PrimaryBeaconConfig();
        auto result = primaryConfig.ltsFromJson(jd["primary"]);
        if (!result) {
          return Error(E_CONFIG, "Failed to parse primary beacon configuration: " + result.error().message);
        }
        primary = primaryConfig;
        } else {
        return Error(E_CONFIG, "Field 'primary' is required in primary mode");
      }
    }

    // Load and validate input beacon configuration
    if (mode == Mode::Input) {
      if (jd.contains("input")) {
        if (!jd["input"].is_object()) {
          return Error(E_CONFIG, "Field 'input' must be an object");
        }
        auto inputConfig = InputBeaconConfig();
        auto result = inputConfig.ltsFromJson(jd["input"]);
        if (!result) {
          return Error(E_CONFIG, "Failed to parse input beacon configuration: " + result.error().message);
        }
        input = inputConfig;
      } else {
        return Error(E_CONFIG, "Field 'input' is required in input mode");
      }
    }

    // Load and validate output beacon configuration
    if (mode == Mode::Output) {
      if (jd.contains("output")) {
        if (!jd["output"].is_object()) {
          return Error(E_CONFIG, "Field 'output' must be an object");
        }
        auto outputConfig = OutputBeaconConfig();
        auto result = outputConfig.ltsFromJson(jd["output"]);
        if (!result) {
          return Error(E_CONFIG, "Failed to parse output beacon configuration: " + result.error().message);
        }
        output = outputConfig;
      } else {
        return Error(E_CONFIG, "Field 'output' is required in output mode");
      }
    }

    return {};
  } catch (const std::exception& e) {
    return Error(E_CONFIG, "Failed to parse run configuration: " + std::string(e.what()));
  }
}

// ============ BeaconServer methods ============

std::string BeaconServer::getModeName(Mode mode) const {
  switch (mode) {
  case Mode::Primary:
    return "Primary";
  case Mode::Input:
    return "Input";
  case Mode::Output:
    return "Output";
  }
  return "Unknown";
}

BeaconServer::Roe<Beacon::InitKeyConfig> BeaconServer::init(const std::string& workDir) {
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
  Beacon::InitKeyConfig kPrivate;
  for (int i = 0; i < 3; i++) {
    auto result = utl::ed25519Generate();
    if (!result) {
      return Error("Failed to generate Ed25519 key: " + result.error().message);
    }
    auto key = result.value();
    kPrivate.genesis.push_back(key.privateKey);
    initConfig.key.genesis.push_back(key.publicKey);

    result = utl::ed25519Generate();
    if (!result) {
      return Error("Failed to generate Ed25519 key: " + result.error().message);
    }
    key = result.value();
    kPrivate.fee.push_back(key.privateKey);
    initConfig.key.fee.push_back(key.publicKey);

    result = utl::ed25519Generate();
    if (!result) {
      return Error("Failed to generate Ed25519 key: " + result.error().message);
    }
    key = result.value();
    kPrivate.reserve.push_back(key.privateKey);
    initConfig.key.reserve.push_back(key.publicKey);
  }
  
  // Call the existing initFromWorkDir method
  auto result = initFromWorkDir(initConfig);
  if (!result) {
    return Error("Failed to initialize beacon: " + result.error().message);
  }
  
  log().info << "Beacon initialized successfully";
  return kPrivate;
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

Service::Roe<void> BeaconServer::run(const std::string &workDir) {
  // Store dataDir for onStart
  workDir_ = workDir;

  log().info << "Running with work directory: " << workDir;
  log().addFileHandler(workDir + "/" + FILE_LOG, logging::getLevel());

  // Call base class run which will invoke onStart() then runLoop() in current thread
  return Service::run();
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
  switch (runFileConfig.mode) {
  case Mode::Primary:
    config_.checkpoint.minSizeBytes = runFileConfig.primary.checkpointSize;
    config_.checkpoint.ageSeconds = runFileConfig.primary.checkpointAge;
    trustedBeaconAddresses_ = runFileConfig.primary.trustedBeacons;
    break;
  case Mode::Input:
    primaryBeaconAddress_ = runFileConfig.input.primaryBeacon;
    break;
  case Mode::Output:
    primaryBeaconAddress_ = runFileConfig.output.primaryBeacon;
    break;
  }
  
  log().info << "Configuration loaded";
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Primary beacon: " << primaryBeaconAddress_;
  log().info << "  Trusted beacons: " << utl::join(trustedBeaconAddresses_, ", ");
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
  fetchServerConfig.handler = [this](int fd, const std::string &request, const network::TcpEndpoint& endpoint) {
    QueuedRequest qr;
    qr.request = request;
    qr.fd = fd;
    requestQueue_.push(std::move(qr));
    log().debug << "Request enqueued (queue size: " << requestQueue_.size() << ")";
  };
  auto serverStarted = fetchServer_.start(fetchServerConfig);
  if (!serverStarted) {
    return Service::Error(-5, "Failed to start FetchServer: " + serverStarted.error().message);
  }

  // Connect to other beacon servers
  if (mode_ != Mode::Primary) {
    auto syncResult = syncWithPrimaryBeacon();
    if (!syncResult) {
      return Service::Error(-6, "Failed to sync with primary beacon: " + syncResult.error().message);
    }
  }

  initHandlers();
  return {};
}

void BeaconServer::initHandlers() {
  requestHandlers_.clear();

  auto& hgs = requestHandlers_[Client::T_REQ_STATUS];
  hgs[Mode::Primary] = [this](const Client::Request &request) { return hStatus(request); };
  hgs[Mode::Output] = [this](const Client::Request &request) { return hStatus(request); };

  hgs[Mode::Input] = [this](const Client::Request &request) { return hUnsupported(request); };

  auto& hgb = requestHandlers_[Client::T_REQ_BLOCK_GET];
  hgb[Mode::Primary] = [this](const Client::Request &request) { return hBlockGet(request); };
  hgb[Mode::Output] = [this](const Client::Request &request) { return hBlockGet(request); };

  hgb[Mode::Input] = [this](const Client::Request &request) { return hUnsupported(request); };

  auto& hga = requestHandlers_[Client::T_REQ_ACCOUNT_GET];
  hga[Mode::Primary] = [this](const Client::Request &request) { return hAccountGet(request); };
  hga[Mode::Output] = [this](const Client::Request &request) { return hAccountGet(request); };

  hga[Mode::Input] = [this](const Client::Request &request) { return hUnsupported(request); };

  auto& hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab[Mode::Primary] = [this](const Client::Request &request) { return hBlockAdd(request); };
  hab[Mode::Input] = [this](const Client::Request &request) { return hBlockAdd(request); };

  hab[Mode::Output] = [this](const Client::Request &request) { return hUnsupported(request); };

  auto& hreg = requestHandlers_[Client::T_REQ_REGISTER];
  hreg[Mode::Primary] = [this](const Client::Request &request) { return hRegister(request); };
  hreg[Mode::Input] = [this](const Client::Request &request) { return hRegister(request); };

  hreg[Mode::Output] = [this](const Client::Request &request) { return hUnsupported(request); };
};

void BeaconServer::onStop() {
  fetchServer_.stop();
  log().info << "BeaconServer resources cleaned up";
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

BeaconServer::Roe<void> BeaconServer::syncWithPrimaryBeacon() {
  if (primaryBeaconAddress_.empty()) {
    return Error(E_CONFIG, "No primary beacon address configured");
  }

  log().info << "Syncing with primary beacon: " << primaryBeaconAddress_;

  if (!client_.setEndpoint(primaryBeaconAddress_)) {
    return Error(E_NETWORK, "Failed to resolve primary beacon address: " + primaryBeaconAddress_);
  }

  auto stateResult = client_.fetchBeaconState();
  if (!stateResult) {
    return Error(E_NETWORK, "Failed to get primary beacon state: " + stateResult.error().message);
  }

  return {};
}

Client::BeaconState BeaconServer::buildStateResponse() const {
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
  
  return state;
}

void BeaconServer::runLoop() {
  log().info << "Request handler thread started";

  QueuedRequest qr;
  while (!isStopSet()) {
    try {
    beacon_.refreshStakeholders();
      // Poll for a request from the queue
      if (requestQueue_.poll(qr)) {
        processQueuedRequest(qr);
      } else {
        // No request available, sleep briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } catch (const std::exception& e) {
      log().error << "Exception in request handler loop: " << e.what();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  log().info << "Request handler thread stopped";
}

void BeaconServer::processQueuedRequest(QueuedRequest& qr) {
  log().debug << "Processing request from queue";
  // Process the request
  std::string response = handleRequest(qr.request);
  
  // Send response back
  auto addResponseResult = fetchServer_.addResponse(qr.fd, response);
  if (!addResponseResult) {
    log().error << "Failed to queue response: " << addResponseResult.error().message;
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
    return hBlockGet(request);
  case Client::T_REQ_BLOCK_ADD:
    return hBlockAdd(request);
  case Client::T_REQ_ACCOUNT_GET:
    return hAccountGet(request);
  case Client::T_REQ_STATUS:
    return hStatus(request);
  case Client::T_REQ_REGISTER:
    return hRegister(request);
  default:
    return Error(E_REQUEST, "Unknown request type: " + std::to_string(request.type));
  }
}

BeaconServer::Roe<std::string> BeaconServer::hBlockGet(const Client::Request &request) {
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

BeaconServer::Roe<std::string> BeaconServer::hBlockAdd(const Client::Request &request) {
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

BeaconServer::Roe<std::string> BeaconServer::hAccountGet(const Client::Request &request) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    return Error(E_REQUEST, "Invalid account get payload: " + request.payload);
  }

  uint64_t accountId = idResult.value();
  auto result = beacon_.getAccount(accountId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get account: " + result.error().message);
  }
  return result.value().ltsToString();
}

BeaconServer::Roe<std::string> BeaconServer::hRegister(const Client::Request &request) {
  network::TcpEndpoint endpoint = network::TcpEndpoint::ltsFromString(request.payload);
  registerServer(endpoint.ltsToString());
  return buildStateResponse().ltsToJson().dump();
}

BeaconServer::Roe<std::string> BeaconServer::hStatus(const Client::Request &request) {
  return buildStateResponse().ltsToJson().dump();
}

BeaconServer::Roe<std::string> BeaconServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST, "Unsupported request type: " + std::to_string(request.type) + " in mode " + getModeName(mode_));
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
