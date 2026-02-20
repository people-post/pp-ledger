#include "BeaconServer.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace pp {

BeaconServer::BeaconServer() {
  redirectLogger("BeaconServer");
  beacon_.redirectLogger(log().getFullName() + ".Beacon");
  client_.redirectLogger(log().getFullName() + ".Client");
}

// ============ InitFileConfig methods ============

nlohmann::json BeaconServer::InitFileConfig::ltsToJson() {
  nlohmann::json j;
  j["slotDuration"] = slotDuration;
  j["slotsPerEpoch"] = slotsPerEpoch;
  j["maxCustomMetaSize"] = maxCustomMetaSize;
  j["maxTransactionsPerBlock"] = maxTransactionsPerBlock;
  j["minFeeCoefficients"] = minFeeCoefficients;
  j["freeCustomMetaSize"] = freeCustomMetaSize;
  j["checkpointMinBlocks"] = checkpointMinBlocks;
  j["checkpointMinAgeSeconds"] = checkpointMinAgeSeconds;
  j["maxValidationTimespanSeconds"] = maxValidationTimespanSeconds;
  return j;
}

BeaconServer::Roe<void>
BeaconServer::InitFileConfig::ltsFromJson(const nlohmann::json &jd) {
  try {
    // Validate JSON is an object
    if (!jd.is_object()) {
      return Error(E_CONFIG, "Configuration must be a JSON object");
    }

    // Load and validate slotDuration
    if (jd.contains("slotDuration")) {
      if (!jd["slotDuration"].is_number_unsigned()) {
        return Error(E_CONFIG,
                     "Field 'slotDuration' must be a positive number");
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
        return Error(E_CONFIG,
                     "Field 'slotsPerEpoch' must be a positive number");
      }
      slotsPerEpoch = jd["slotsPerEpoch"].get<uint64_t>();
      if (slotsPerEpoch == 0) {
        return Error(E_CONFIG, "Field 'slotsPerEpoch' must be greater than 0");
      }
    } else {
      slotsPerEpoch = DEFAULT_SLOTS_PER_EPOCH;
    }

    // Load and validate maxCustomMetaSize
    if (jd.contains("maxCustomMetaSize")) {
      if (!jd["maxCustomMetaSize"].is_number_unsigned()) {
        return Error(E_CONFIG,
                     "Field 'maxCustomMetaSize' must be a positive number");
      }
      maxCustomMetaSize = jd["maxCustomMetaSize"].get<uint64_t>();
      if (maxCustomMetaSize == 0) {
        return Error(E_CONFIG,
                     "Field 'maxCustomMetaSize' must be greater than 0");
      }
    } else {
      maxCustomMetaSize = DEFAULT_MAX_CUSTOM_META_SIZE;
    }

    // Load and validate maxTransactionsPerBlock
    if (jd.contains("maxTransactionsPerBlock")) {
      if (!jd["maxTransactionsPerBlock"].is_number_unsigned()) {
        return Error(
            E_CONFIG,
            "Field 'maxTransactionsPerBlock' must be a positive number");
      }
      maxTransactionsPerBlock = jd["maxTransactionsPerBlock"].get<uint64_t>();
      if (maxTransactionsPerBlock == 0) {
        return Error(E_CONFIG,
                     "Field 'maxTransactionsPerBlock' must be greater than 0");
      }
    } else {
      maxTransactionsPerBlock = DEFAULT_MAX_TRANSACTIONS_PER_BLOCK;
    }

    // Load and validate minFeeCoefficients
    if (jd.contains("minFeeCoefficients")) {
      if (!jd["minFeeCoefficients"].is_array()) {
        return Error(E_CONFIG, "Field 'minFeeCoefficients' must be an array");
      }
      minFeeCoefficients.clear();
      for (const auto &value : jd["minFeeCoefficients"]) {
        if (!value.is_number_unsigned()) {
          return Error(
              E_CONFIG,
              "Field 'minFeeCoefficients' values must be positive numbers");
        }
        const uint64_t coefficient = value.get<uint64_t>();
        if (coefficient > std::numeric_limits<uint16_t>::max()) {
          return Error(
              E_CONFIG,
              "Field 'minFeeCoefficients' values must be <= 65535");
        }
        minFeeCoefficients.push_back(static_cast<uint16_t>(coefficient));
      }
      if (minFeeCoefficients.empty()) {
        return Error(E_CONFIG,
                     "Field 'minFeeCoefficients' must not be empty");
      }
    } else {
      uint64_t minFeePerTransaction = DEFAULT_MIN_FEE_COEFF_A;
      uint64_t minFeePerCustomMetaMiB = DEFAULT_MIN_FEE_COEFF_B;

      if (jd.contains("minFeePerTransaction")) {
        if (!jd["minFeePerTransaction"].is_number_unsigned()) {
          return Error(
              E_CONFIG,
              "Field 'minFeePerTransaction' must be a positive number");
        }
        minFeePerTransaction = jd["minFeePerTransaction"].get<uint64_t>();
      }

      if (jd.contains("minFeePerCustomMetaMiB")) {
        if (!jd["minFeePerCustomMetaMiB"].is_number_unsigned()) {
          return Error(
              E_CONFIG,
              "Field 'minFeePerCustomMetaMiB' must be a positive number");
        }
        minFeePerCustomMetaMiB = jd["minFeePerCustomMetaMiB"].get<uint64_t>();
      }

      if (minFeePerTransaction > std::numeric_limits<uint16_t>::max() ||
          minFeePerCustomMetaMiB > std::numeric_limits<uint16_t>::max()) {
        return Error(
            E_CONFIG,
            "Legacy fee fields must be <= 65535 to map to minFeeCoefficients");
      }

      minFeeCoefficients = {
          static_cast<uint16_t>(minFeePerTransaction),
          static_cast<uint16_t>(minFeePerCustomMetaMiB),
          DEFAULT_MIN_FEE_COEFF_C,
      };
    }

    // Load and validate freeCustomMetaSize
    if (jd.contains("freeCustomMetaSize")) {
      if (!jd["freeCustomMetaSize"].is_number_unsigned()) {
        return Error(E_CONFIG,
                     "Field 'freeCustomMetaSize' must be a positive number");
      }
      freeCustomMetaSize = jd["freeCustomMetaSize"].get<uint64_t>();
      if (freeCustomMetaSize > maxCustomMetaSize) {
        return Error(
            E_CONFIG,
            "Field 'freeCustomMetaSize' must be less than or equal to "
            "'maxCustomMetaSize'");
      }
    } else {
      freeCustomMetaSize = DEFAULT_FREE_CUSTOM_META_SIZE;
      if (freeCustomMetaSize > maxCustomMetaSize) {
        freeCustomMetaSize = maxCustomMetaSize;
      }
    }

    // Load and validate checkpointMinBlocks
    if (jd.contains("checkpointMinBlocks")) {
      if (!jd["checkpointMinBlocks"].is_number_unsigned()) {
        return Error(E_CONFIG,
                     "Field 'checkpointMinBlocks' must be a positive number");
      }
      checkpointMinBlocks = jd["checkpointMinBlocks"].get<uint64_t>();
    } else {
      checkpointMinBlocks = DEFAULT_CHECKPOINT_MIN_BLOCKS;
    }

    // Load and validate checkpointMinAgeSeconds
    if (jd.contains("checkpointMinAgeSeconds")) {
      if (!jd["checkpointMinAgeSeconds"].is_number_unsigned()) {
        return Error(
            E_CONFIG,
            "Field 'checkpointMinAgeSeconds' must be a positive number");
      }
      checkpointMinAgeSeconds = jd["checkpointMinAgeSeconds"].get<uint64_t>();
    } else {
      checkpointMinAgeSeconds = DEFAULT_CHECKPOINT_MIN_AGE_SECONDS;
    }

    // Load and validate maxValidationTimespanSeconds (must be > 0)
    if (jd.contains("maxValidationTimespanSeconds")) {
      if (!jd["maxValidationTimespanSeconds"].is_number_unsigned()) {
        return Error(
            E_CONFIG,
            "Field 'maxValidationTimespanSeconds' must be a positive number");
      }
      maxValidationTimespanSeconds =
          jd["maxValidationTimespanSeconds"].get<uint64_t>();
      if (maxValidationTimespanSeconds == 0) {
        return Error(E_CONFIG,
                     "Field 'maxValidationTimespanSeconds' must be greater than 0");
      }
    } else {
      maxValidationTimespanSeconds = DEFAULT_MAX_VALIDATION_TIMESPAN_SECONDS;
    }

    return {};
  } catch (const std::exception &e) {
    return Error(E_CONFIG, "Failed to parse init configuration: " +
                               std::string(e.what()));
  }
}

// ============ RunFileConfig methods ============

nlohmann::json BeaconServer::RunFileConfig::ltsToJson() {
  nlohmann::json j;
  j["host"] = host;
  j["port"] = port;
  j["whitelist"] = whitelist;
  return j;
}

BeaconServer::Roe<void>
BeaconServer::RunFileConfig::ltsFromJson(const nlohmann::json &jd) {
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

    // Load and validate whitelist
    if (jd.contains("whitelist")) {
      if (!jd["whitelist"].is_array()) {
        return Error(E_CONFIG, "Field 'whitelist' must be an array");
      }
      whitelist = jd["whitelist"].get<std::vector<std::string>>();
    }

    return {};
  } catch (const std::exception &e) {
    return Error(E_CONFIG,
                 "Failed to parse run configuration: " + std::string(e.what()));
  }
}

// ============ BeaconServer methods ============

BeaconServer::Roe<Beacon::InitKeyConfig>
BeaconServer::init(const std::string &workDir) {
  log().info << "Initializing new beacon with work directory: " << workDir;

  std::filesystem::path workDirPath(workDir);
  std::filesystem::path initConfigPath = workDirPath / FILE_INIT_CONFIG;
  std::filesystem::path signaturePath = workDirPath / FILE_SIGNATURE;

  if (std::filesystem::exists(workDirPath)) {
    if (!std::filesystem::exists(signaturePath)) {
      return Error("Work directory not recognized, please remove it manually "
                   "and try again");
    }
  } else {
    // Create work directory if it doesn't exist (to write config file)
    std::filesystem::create_directories(workDirPath);
    // Create signature file to mark work directory as recognized
    auto result = utl::writeToNewFile(signaturePath.string(), "");
    if (!result) {
      return Error("Failed to create signature file: " +
                   result.error().message);
    }
    log().info << "Created work directory: " << workDir;
  }

  // Create or load FILE_INIT_CONFIG using InitFileConfig
  InitFileConfig initFileConfig;

  if (!std::filesystem::exists(initConfigPath)) {
    log().info << "Creating " << FILE_INIT_CONFIG << " with default parameters";

    // Use default values from InitFileConfig struct
    nlohmann::json defaultConfig = initFileConfig.ltsToJson();

    auto result =
        utl::writeToNewFile(initConfigPath.string(), defaultConfig.dump(2));
    if (!result) {
      return Error("Failed to create " + std::string(FILE_INIT_CONFIG) + ": " +
                   result.error().message);
    }

    log().info << "Created: " << initConfigPath.string();
  } else {
    log().info << "Found existing " << FILE_INIT_CONFIG;
  }

  // Load configuration from FILE_INIT_CONFIG
  log().info << "Loading configuration from: " << initConfigPath.string();

  auto jsonResult = utl::loadJsonFile(initConfigPath.string());
  if (!jsonResult) {
    return Error("Failed to load init config file: " +
                 jsonResult.error().message);
  }

  nlohmann::json config = jsonResult.value();

  // Use InitFileConfig to parse configuration
  auto parseResult = initFileConfig.ltsFromJson(config);
  if (!parseResult) {
    return Error("Failed to parse init config file: " +
                 parseResult.error().message);
  }

  log().info << "Configuration:";
  log().info << "  Slot duration: " << initFileConfig.slotDuration
             << " seconds";
  log().info << "  Slots per epoch: " << initFileConfig.slotsPerEpoch;
  log().info << "  Max custom meta size: "
             << initFileConfig.maxCustomMetaSize;
  log().info << "  Max transactions per block: "
             << initFileConfig.maxTransactionsPerBlock;

  // Prepare init configuration
  Beacon::InitConfig initConfig;
  initConfig.workDir = workDir + "/" + DIR_DATA;
  initConfig.chain.slotDuration = initFileConfig.slotDuration;
  initConfig.chain.slotsPerEpoch = initFileConfig.slotsPerEpoch;
  initConfig.chain.maxCustomMetaSize = initFileConfig.maxCustomMetaSize;
  initConfig.chain.maxTransactionsPerBlock =
      initFileConfig.maxTransactionsPerBlock;
    initConfig.chain.minFeeCoefficients = initFileConfig.minFeeCoefficients;
  initConfig.chain.freeCustomMetaSize = initFileConfig.freeCustomMetaSize;
  initConfig.chain.checkpoint.minBlocks = initFileConfig.checkpointMinBlocks;
  initConfig.chain.checkpoint.minAgeSeconds =
      initFileConfig.checkpointMinAgeSeconds;
  initConfig.chain.maxValidationTimespanSeconds =
      initFileConfig.maxValidationTimespanSeconds;

  // Generate keypairs; pass KeyPairs to beacon for genesis signing and
  // checkpoint public keys
  for (int i = 0; i < 3; i++) {
    auto result = utl::ed25519Generate();
    if (!result) {
      return Error("Failed to generate Ed25519 key: " + result.error().message);
    }
    initConfig.key.genesis.push_back(result.value());

    result = utl::ed25519Generate();
    if (!result) {
      return Error("Failed to generate Ed25519 key: " + result.error().message);
    }
    initConfig.key.fee.push_back(result.value());

    result = utl::ed25519Generate();
    if (!result) {
      return Error("Failed to generate Ed25519 key: " + result.error().message);
    }
    initConfig.key.reserve.push_back(result.value());

    result = utl::ed25519Generate();
    if (!result) {
      return Error("Failed to generate Ed25519 key: " + result.error().message);
    }
    initConfig.key.recycle.push_back(result.value());
  }

  auto result = initFromWorkDir(initConfig);
  if (!result) {
    return Error("Failed to initialize beacon: " + result.error().message);
  }

  log().info << "Beacon initialized successfully";
  return initConfig.key;
}

BeaconServer::Roe<void>
BeaconServer::initFromWorkDir(const Beacon::InitConfig &config) {
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

Service::Roe<void> BeaconServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(getWorkDir()) / FILE_CONFIG;
  std::string configPathStr = configPath.string();

  // Create default FILE_CONFIG if it doesn't exist using RunFileConfig
  RunFileConfig runFileConfig;

  if (!std::filesystem::exists(configPath)) {
    log().info << "No " << FILE_CONFIG
               << " found, creating with default values";

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
      return Service::Error(-3, "Failed to load config file: " +
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
  config_.network.whitelist = runFileConfig.whitelist;

  log().info << "Configuration loaded";
  log().info << "  Endpoint: " << config_.network.endpoint;
  log().info << "  Whitelisted beacons: "
             << utl::join(config_.network.whitelist, ", ");

  // Initialize beacon core with mount config
  Beacon::MountConfig mountConfig;
  mountConfig.workDir = getWorkDir() + "/" + DIR_DATA;

  auto beaconMount = beacon_.mount(mountConfig);
  if (!beaconMount) {
    return Service::Error(-4, "Failed to mount Beacon: " +
                                  beaconMount.error().message);
  }

  log().info << "Beacon core initialized";

  auto serverStarted = startFetchServer(config_.network.endpoint);
  if (!serverStarted) {
    return Service::Error(-5, "Failed to start FetchServer: " +
                                  serverStarted.error().message);
  }

  initHandlers();
  return {};
}

void BeaconServer::customizeFetchServerConfig(
    network::FetchServer::Config &config) {
  config.whitelist = config_.network.whitelist;
}

void BeaconServer::initHandlers() {
  requestHandlers_.clear();

  auto &hgs = requestHandlers_[Client::T_REQ_STATUS];
  hgs = [this](const Client::Request &request) { return hStatus(request); };

  auto &hts = requestHandlers_[Client::T_REQ_TIMESTAMP];
  hts = [this](const Client::Request &request) { return hTimestamp(request); };

  auto &hgb = requestHandlers_[Client::T_REQ_BLOCK_GET];
  hgb = [this](const Client::Request &request) { return hBlockGet(request); };

  auto &hga = requestHandlers_[Client::T_REQ_ACCOUNT_GET];
  hga = [this](const Client::Request &request) { return hAccountGet(request); };

  auto &hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab = [this](const Client::Request &request) { return hBlockAdd(request); };

  auto &hreg = requestHandlers_[Client::T_REQ_REGISTER];
  hreg = [this](const Client::Request &request) { return hRegister(request); };

  auto &hml = requestHandlers_[Client::T_REQ_MINER_LIST];
  hml = [this](const Client::Request &request) { return hMinerList(request); };
};

void BeaconServer::onStop() {
  Server::onStop();
  log().info << "BeaconServer resources cleaned up";
}

void BeaconServer::registerServer(const Client::MinerInfo &minerInfo) {
  mMiners_[minerInfo.id] = minerInfo;
  log().debug << "Updated miner record: " << minerInfo.id << " " << minerInfo.endpoint;
}

Client::BeaconState BeaconServer::buildStateResponse() const {
  int64_t currentTimestamp =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

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

  while (!isStopSet()) {
    try {
      // Update beacon state
      beacon_.refresh();

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

  log().info << "Request handler thread stopped";
}

std::string BeaconServer::handleParsedRequest(const Client::Request &request) {
  log().debug << "Handling request: " << request.type;
  auto it = requestHandlers_.find(request.type);
  Roe<std::string> result = (it != requestHandlers_.end())
                                ? it->second(request)
                                : hUnsupported(request);
  if (!result) {
    return Server::packResponse(1, result.error().message);
  }
  return Server::packResponse(result.value());
}

BeaconServer::Roe<std::string>
BeaconServer::hBlockGet(const Client::Request &request) {
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

BeaconServer::Roe<std::string>
BeaconServer::hBlockAdd(const Client::Request &request) {
  Ledger::ChainNode block;
  if (!block.ltsFromString(request.payload)) {
    return Error(E_REQUEST, "Failed to deserialize block: " + request.payload);
  }
  auto result = beacon_.addBlock(block);
  if (!result) {
    return Error(E_REQUEST, "Failed to add block: " + result.error().message);
  }
  return {"Block added"};
}

BeaconServer::Roe<std::string>
BeaconServer::hAccountGet(const Client::Request &request) {
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

BeaconServer::Roe<std::string>
BeaconServer::hRegister(const Client::Request &request) {
  Client::MinerInfo minerInfo;
  if (!minerInfo.ltsFromJson(nlohmann::json::parse(request.payload))) {
    return Error(E_REQUEST, "Failed to parse miner info: " + request.payload);
  }
  registerServer(minerInfo);
  return buildStateResponse().ltsToJson().dump();
}

BeaconServer::Roe<std::string>
BeaconServer::hStatus(const Client::Request &request) {
  return buildStateResponse().ltsToJson().dump();
}

BeaconServer::Roe<std::string>
BeaconServer::hTimestamp(const Client::Request &request) {
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return utl::binaryPack(nowMs);
}

BeaconServer::Roe<std::string>
BeaconServer::hMinerList(const Client::Request &request) {
  nlohmann::json j = nlohmann::json::array();
  for (const auto &[id, info] : mMiners_) {
    j.push_back(info.ltsToJson());
  }
  return j.dump();
}

BeaconServer::Roe<std::string>
BeaconServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST,
               "Unsupported request type: " + std::to_string(request.type));
}

} // namespace pp
