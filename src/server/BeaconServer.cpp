#include "BeaconServer.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include "common/io/Json.h"

namespace pp {
namespace {
using pp::common::Array;
using pp::common::ArrayPtr;
using pp::common::Object;
using pp::common::ObjectPtr;
using pp::common::Value;
using pp::common::asNonNegInt;
using pp::common::asObject;
using pp::common::asString;
using pp::common::io::valueToJsonString;

pp::Roe<std::string> encodeObjectPretty(const Object &o) {
  auto r = valueToJsonString(Value(std::make_shared<Object>(o)), 2);
  if (!r.isOk()) {
    return pp::Error(r.error().message);
  }
  return r.value();
}
} // namespace

// ============ InitFileConfig methods ============

Object BeaconServer::InitFileConfig::ltsToJson() {
  Object j;
  j.setJsonUInt("slotDuration", slotDuration);
  j.setJsonUInt("slotsPerEpoch", slotsPerEpoch);
  j.setJsonUInt("maxCustomMetaSize", maxCustomMetaSize);
  j.setJsonUInt("maxTransactionsPerBlock", maxTransactionsPerBlock);
  {
    std::vector<Value> coeffs;
    for (uint16_t c : minFeeCoefficients) {
      coeffs.push_back(static_cast<int64_t>(c));
    }
    j.set("minFeeCoefficients", Object::array(std::move(coeffs)));
  }
  j.setJsonUInt("freeCustomMetaSize", freeCustomMetaSize);
  j.setJsonUInt("checkpointMinBlocks", checkpointMinBlocks);
  j.setJsonUInt("checkpointMinAgeSeconds", checkpointMinAgeSeconds);
  j.setJsonUInt("maxValidationTimespanSeconds", maxValidationTimespanSeconds);
  return j;
}

BeaconServer::Roe<void>
BeaconServer::InitFileConfig::ltsFromJson(const Object &jd) {
  auto readU64 = [&](const char *field, uint64_t &out, bool required,
                     bool allowZero) -> Roe<void> {
    if (!jd.contains(field)) {
      if (required) {
        return Error(E_CONFIG, std::string("Field '") + field + "' is required");
      }
      return {};
    }
    auto v = jd.getNonNegInt(field);
    if (!v) {
      return Error(E_CONFIG, std::string("Field '") + field +
                                 "' must be a non-negative integer");
    }
    if (!allowZero && *v == 0) {
      return Error(E_CONFIG, std::string("Field '") + field +
                                 "' must be greater than 0");
    }
    out = *v;
    return {};
  };

  if (jd.contains("slotDuration")) {
    if (auto r = readU64("slotDuration", slotDuration, true, false); !r) return r;
  } else {
    slotDuration = DEFAULT_SLOT_DURATION;
  }

  if (jd.contains("slotsPerEpoch")) {
    if (auto r = readU64("slotsPerEpoch", slotsPerEpoch, true, false); !r) return r;
  } else {
    slotsPerEpoch = DEFAULT_SLOTS_PER_EPOCH;
  }

  if (jd.contains("maxCustomMetaSize")) {
    if (auto r = readU64("maxCustomMetaSize", maxCustomMetaSize, true, false); !r) return r;
  } else {
    maxCustomMetaSize = DEFAULT_MAX_CUSTOM_META_SIZE;
  }

  if (jd.contains("maxTransactionsPerBlock")) {
    if (auto r = readU64("maxTransactionsPerBlock", maxTransactionsPerBlock, true, false); !r)
      return r;
  } else {
    maxTransactionsPerBlock = DEFAULT_MAX_TRANSACTIONS_PER_BLOCK;
  }

  if (jd.contains("minFeeCoefficients")) {
    const Array *arr = jd.getArray("minFeeCoefficients");
    if (!arr) {
      return Error(E_CONFIG, "Field 'minFeeCoefficients' must be an array");
    }
    minFeeCoefficients.clear();
    for (const auto &value : arr->elements) {
      auto coefficient = asNonNegInt(value);
      if (!coefficient) {
        return Error(E_CONFIG,
                     "Field 'minFeeCoefficients' values must be non-negative integers");
      }
      if (*coefficient > std::numeric_limits<uint16_t>::max()) {
        return Error(E_CONFIG, "Field 'minFeeCoefficients' values must be <= 65535");
      }
      minFeeCoefficients.push_back(static_cast<uint16_t>(*coefficient));
    }
    if (minFeeCoefficients.empty()) {
      return Error(E_CONFIG, "Field 'minFeeCoefficients' must not be empty");
    }
  } else {
    uint64_t minFeePerTransaction = DEFAULT_MIN_FEE_COEFF_A;
    uint64_t minFeePerCustomMetaMiB =
        static_cast<uint64_t>(DEFAULT_MIN_FEE_COEFF_B) * 1024ULL;
    if (jd.contains("minFeePerTransaction")) {
      auto v = jd.getNonNegInt("minFeePerTransaction");
      if (!v) {
        return Error(E_CONFIG, "Field 'minFeePerTransaction' must be a non-negative integer");
      }
      minFeePerTransaction = *v;
    }
    if (jd.contains("minFeePerCustomMetaMiB")) {
      auto v = jd.getNonNegInt("minFeePerCustomMetaMiB");
      if (!v) {
        return Error(E_CONFIG, "Field 'minFeePerCustomMetaMiB' must be a non-negative integer");
      }
      minFeePerCustomMetaMiB = *v;
    }
    const uint64_t minFeePerCustomMetaKiB = minFeePerCustomMetaMiB / 1024ULL;
    if (minFeePerTransaction > std::numeric_limits<uint16_t>::max() ||
        minFeePerCustomMetaKiB > std::numeric_limits<uint16_t>::max()) {
      return Error(E_CONFIG,
                   "Legacy fee fields must be <= 65535 to map to minFeeCoefficients");
    }
    minFeeCoefficients = {
        static_cast<uint16_t>(minFeePerTransaction),
        static_cast<uint16_t>(minFeePerCustomMetaKiB),
        DEFAULT_MIN_FEE_COEFF_C,
    };
  }

  if (jd.contains("freeCustomMetaSize")) {
    if (auto r = readU64("freeCustomMetaSize", freeCustomMetaSize, true, true); !r) return r;
    if (freeCustomMetaSize > maxCustomMetaSize) {
      return Error(E_CONFIG,
                   "Field 'freeCustomMetaSize' must be less than or equal to "
                   "'maxCustomMetaSize'");
    }
  } else {
    freeCustomMetaSize = DEFAULT_FREE_CUSTOM_META_SIZE;
    if (freeCustomMetaSize > maxCustomMetaSize) {
      freeCustomMetaSize = maxCustomMetaSize;
    }
  }

  if (jd.contains("checkpointMinBlocks")) {
    if (auto r = readU64("checkpointMinBlocks", checkpointMinBlocks, true, true); !r) return r;
  } else {
    checkpointMinBlocks = DEFAULT_CHECKPOINT_MIN_BLOCKS;
  }

  if (jd.contains("checkpointMinAgeSeconds")) {
    if (auto r = readU64("checkpointMinAgeSeconds", checkpointMinAgeSeconds, true, true); !r)
      return r;
  } else {
    checkpointMinAgeSeconds = DEFAULT_CHECKPOINT_MIN_AGE_SECONDS;
  }

  if (jd.contains("maxValidationTimespanSeconds")) {
    if (auto r = readU64("maxValidationTimespanSeconds", maxValidationTimespanSeconds, true,
                         false);
        !r)
      return r;
  } else {
    maxValidationTimespanSeconds = DEFAULT_MAX_VALIDATION_TIMESPAN_SECONDS;
  }

  return {};
}

// ============ RunFileConfig methods ============

Object BeaconServer::RunFileConfig::ltsToJson() {
  Object j;
  j.set("host", host);
  j.setJsonUInt("port", port);
  j.setJsonUInt("dhtPort", dhtPort);
  std::vector<Value> wl;
  for (const auto &w : whitelist) {
    wl.push_back(w);
  }
  j.set("whitelist", Object::array(std::move(wl)));
  return j;
}

BeaconServer::Roe<void>
BeaconServer::RunFileConfig::ltsFromJson(const Object &jd) {
  auto hostOpt = jd.getString("host");
  if (!hostOpt) {
    return Error(E_CONFIG, jd.contains("host") ? "Field 'host' must be a string"
                                               : "Field 'host' is required");
  }
  host = *hostOpt;
  if (host.empty()) {
    return Error(E_CONFIG, "Field 'host' cannot be empty");
  }

  auto portValue = jd.getNonNegInt("port");
  if (!portValue) {
    return Error(E_CONFIG, jd.contains("port")
                               ? "Field 'port' must be a non-negative integer"
                               : "Field 'port' is required");
  }
  if (*portValue == 0 || *portValue > 65535) {
    return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
  }
  port = static_cast<uint16_t>(*portValue);

  auto dhtPortValue = jd.getNonNegInt("dhtPort");
  if (!dhtPortValue) {
    return Error(E_CONFIG, jd.contains("dhtPort")
                               ? "Field 'dhtPort' must be a non-negative integer"
                               : "Field 'dhtPort' is required");
  }
  if (*dhtPortValue > 65535) {
    return Error(E_CONFIG, "Field 'dhtPort' must be between 0 and 65535");
  }
  dhtPort = static_cast<uint16_t>(*dhtPortValue);

  if (jd.contains("whitelist")) {
    const Array *arr = jd.getArray("whitelist");
    if (!arr) {
      return Error(E_CONFIG, "Field 'whitelist' must be an array");
    }
    whitelist.clear();
    for (const auto &el : arr->elements) {
      auto s = asString(el);
      if (!s) {
        return Error(E_CONFIG, "Field 'whitelist' elements must be strings");
      }
      whitelist.push_back(*s);
    }
  }

  return {};
}

// ============ BeaconServer methods ============

BeaconServer::BeaconServer() {
  redirectLogger("BeaconServer");
  beacon_.redirectLogger(log().getFullName() + ".Beacon");
  client_.redirectLogger(log().getFullName() + ".Client");
  dhtRunner_.redirectLogger(log().getFullName() + ".Dht");
}

BeaconServer::Roe<Beacon::InitKeyConfig>
BeaconServer::init(const std::string &workDir) {
  log().info << "Initializing new beacon with work directory: " << workDir;

  std::filesystem::path workDirPath(workDir);
  std::filesystem::path initConfigPath = workDirPath / FILE_INIT_CONFIG;

  auto ensured = ensureWorkDirectory(workDir, FILE_SIGNATURE);
  if (!ensured) {
    return Error(ensured.error().code, ensured.error().message);
  }
  if (!std::filesystem::exists(workDirPath / FILE_INIT_CONFIG) &&
      !std::filesystem::exists(workDirPath / FILE_CONFIG)) {
    log().info << "Created work directory: " << workDir;
  }

  // Create or load FILE_INIT_CONFIG using InitFileConfig
  InitFileConfig initFileConfig;

  if (!std::filesystem::exists(initConfigPath)) {
    log().info << "Creating " << FILE_INIT_CONFIG << " with default parameters";

    // Use default values from InitFileConfig struct
    auto encoded = encodeObjectPretty(initFileConfig.ltsToJson());
    if (!encoded) {
      return Error("Failed to encode " + std::string(FILE_INIT_CONFIG) + ": " +
                   encoded.error().message);
    }
    auto result =
        utl::writeToNewFile(initConfigPath.string(), encoded.value());
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

  auto parseResult = initFileConfig.ltsFromJson(jsonResult.value());
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
  if (initFileConfig.freeCustomMetaSize >
      std::numeric_limits<uint32_t>::max()) {
    return Error("freeCustomMetaSize exceeds uint32_t range");
  }
  initConfig.chain.freeCustomMetaSize =
      static_cast<uint32_t>(initFileConfig.freeCustomMetaSize);
  initConfig.chain.checkpoint.minBlocks = initFileConfig.checkpointMinBlocks;
  initConfig.chain.checkpoint.minAgeSeconds =
      initFileConfig.checkpointMinAgeSeconds;
  initConfig.chain.maxValidationTimespanSeconds =
      initFileConfig.maxValidationTimespanSeconds;

  // Generate keypairs; pass KeyPairs to beacon for genesis signing and
  // checkpoint public keys
  for (int i = 0; i < 3; i++) {
    auto result = utl::mlDsaGenerate();
    if (!result) {
      return Error("Failed to generate ML-DSA-65 key: " + result.error().message);
    }
    initConfig.key.genesis.push_back(result.value());

    result = utl::mlDsaGenerate();
    if (!result) {
      return Error("Failed to generate ML-DSA-65 key: " + result.error().message);
    }
    initConfig.key.fee.push_back(result.value());

    result = utl::mlDsaGenerate();
    if (!result) {
      return Error("Failed to generate ML-DSA-65 key: " + result.error().message);
    }
    initConfig.key.reserve.push_back(result.value());

    result = utl::mlDsaGenerate();
    if (!result) {
      return Error("Failed to generate ML-DSA-65 key: " + result.error().message);
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
    auto encoded = encodeObjectPretty(runFileConfig.ltsToJson());
    if (!encoded) {
      return Service::Error(-2, "Failed to encode " + std::string(FILE_CONFIG) +
                                    ": " + encoded.error().message);
    }

    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(-2, "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << encoded.value() << std::endl;
    configFile.close();

    log().info << "Created " << FILE_CONFIG << " at: " << configPathStr;
  } else {
    // Load existing configuration
    auto jsonResult = utl::loadJsonFile(configPathStr);
    if (!jsonResult) {
      return Service::Error(-3, "Failed to load config file: " +
                                    jsonResult.error().message);
    }

    auto parseResult = runFileConfig.ltsFromJson(jsonResult.value());
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

  // Start DHT (beacon is the bootstrapping peer; no bootstrap endpoints)
  network::DhtRunner::Config dhtConfig;
  dhtConfig.bootstrapEndpoints = {};
  dhtConfig.dhtPort = runFileConfig.dhtPort;
  dhtConfig.myTcpPort = config_.network.endpoint.port;
  dhtConfig.networkId = network::DhtRunner::getDefaultNetworkId();
  dhtConfig.nodeIdPath = getWorkDir() + "/dht-node.id";
  auto dhtStart = dhtRunner_.start(dhtConfig);
  if (!dhtStart) {
    return Service::Error(E_NETWORK, "Failed to start DHT: " +
                                        dhtStart.error().message);
  }

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
    network::FetchServer::Config& config) {
  config.whitelist = config_.network.whitelist;
  config.security = network::SecurityConfig::trustedDefaults();
  config.performance.maxRequestQueueSize = 8192;
}

void BeaconServer::initHandlers() {
  requestHandlers_.clear();

  auto &hgs = requestHandlers_[Client::T_REQ_STATUS];
  hgs = [this](const Client::Request &request) { return hStatus(request); };

  auto &hcs = requestHandlers_[Client::T_REQ_CALIBRATION];
  hcs = [this](const Client::Request &request) { return hCalibration(request); };

  auto &hgb = requestHandlers_[Client::T_REQ_BLOCK_GET];
  hgb = [this](const Client::Request &request) { return hBlockGet(request); };

  auto &hga = requestHandlers_[Client::T_REQ_ACCOUNT_GET];
  hga = [this](const Client::Request &request) { return hAccountGet(request); };

  auto &htx = requestHandlers_[Client::T_REQ_TX_GET_BY_WALLET];
  htx = [this](const Client::Request &request) { return hTxGetByWallet(request); };

  auto &htxi = requestHandlers_[Client::T_REQ_TX_GET_BY_INDEX];
  htxi = [this](const Client::Request &request) { return hTxGetByIndex(request); };

  auto &hab = requestHandlers_[Client::T_REQ_BLOCK_ADD];
  hab = [this](const Client::Request &request) { return hBlockAdd(request); };

  auto &hreg = requestHandlers_[Client::T_REQ_REGISTER];
  hreg = [this](const Client::Request &request) { return hRegister(request); };

  auto &hml = requestHandlers_[Client::T_REQ_MINER_LIST];
  hml = [this](const Client::Request &request) { return hMinerList(request); };
};

void BeaconServer::onStop() {
  dhtRunner_.stop();
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
  const auto checkpoint = beacon_.getCheckpoint();
  state.checkpointId = checkpoint.lastId;  // Use lastId so that miners can replay blocks to current checkpoint
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
      beacon_.refresh();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } catch (const std::exception& e) {
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
  auto result = beacon_.readBlock(blockId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get block: " + result.error().message);
  }

  return result.value().ltsToString();
}

BeaconServer::Roe<std::string>
BeaconServer::hTxGetByWallet(const Client::Request &request) {
  auto reqResult = utl::binaryUnpack<Client::TxGetByWalletRequest>(request.payload);
  if (!reqResult) {
    return Error(E_REQUEST, "Failed to deserialize request: " + reqResult.error().message);
  }
  auto &req = reqResult.value();
  auto result = beacon_.findTransactionsByWalletId(req.walletId, req.beforeBlockId);
  if (!result) {
    return Error(E_REQUEST, "Failed to get transactions: " + result.error().message);
  }
  Client::TxGetByWalletResponse response;
  response.transactions = result.value();
  response.nextBlockId = req.beforeBlockId;
  return utl::binaryPack(response);
}

BeaconServer::Roe<std::string>
BeaconServer::hTxGetByIndex(const Client::Request &request) {
  auto reqResult = utl::binaryUnpack<Client::TxGetByIndexRequest>(request.payload);
  if (!reqResult) {
    return Error(E_REQUEST, "Failed to deserialize request: " + reqResult.error().message);
  }
  auto &req = reqResult.value();
  auto result = beacon_.findTransactionByIndex(req.txIndex);
  if (!result) {
    return Error(E_REQUEST, "Failed to get transaction: " + result.error().message);
  }
  return utl::binaryPack(result.value());
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
  auto unpacked = utl::binaryUnpack<pp::common::Meta>(request.payload);
  if (!unpacked) {
    return Error(E_REQUEST,
                 "Failed to unpack miner info Meta: " + unpacked.error().message);
  }
  Client::MinerInfo minerInfo;
  auto parsed = minerInfo.ltsFromMeta(unpacked.value());
  if (!parsed) {
    return Error(E_REQUEST, parsed.error().message);
  }
  registerServer(minerInfo);
  return utl::binaryPack(buildStateResponse().ltsToMeta());
}

BeaconServer::Roe<std::string>
BeaconServer::hStatus(const Client::Request & /*request*/) {
  return utl::binaryPack(buildStateResponse().ltsToMeta());
}

BeaconServer::Roe<std::string>
BeaconServer::hCalibration(const Client::Request & /*request*/) {
  Client::CalibrationResponse response;
  response.msTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  response.nextBlockId = beacon_.getNextBlockId();
  return utl::binaryPack(response);
}

BeaconServer::Roe<std::string>
BeaconServer::hMinerList(const Client::Request & /*request*/) {
  std::vector<pp::common::Meta> list;
  list.reserve(mMiners_.size());
  for (const auto &[id, info] : mMiners_) {
    list.push_back(info.ltsToMeta());
  }
  return utl::binaryPack(list);
}

BeaconServer::Roe<std::string>
BeaconServer::hUnsupported(const Client::Request &request) {
  return Error(E_REQUEST,
               "Unsupported request type: " + std::to_string(request.type));
}

} // namespace pp
