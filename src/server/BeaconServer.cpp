#include "BeaconServer.h"
#include "BlockAddPolicy.h"
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

namespace {

/** `entry.publicKeys` (hex ML-DSA-65) and optional `minSignatures` (1..count; default all). */
BeaconServer::Roe<Beacon::AccountKeys> parseAccountKeys(const Object &entry, const std::string &where) {
  const Array *keys = entry.getArray("publicKeys");
  if (!keys || keys->elements.empty()) {
    return BeaconServer::Error(BeaconServer::E_CONFIG, where + ".publicKeys must be a non-empty array of hex keys");
  }
  Beacon::AccountKeys out;
  for (const auto &keyValue : keys->elements) {
    auto hex = pp::common::asString(keyValue);
    std::string text = hex ? *hex : std::string{};
    if (text.rfind("0x", 0) == 0) {
      text = text.substr(2);
    }
    std::string key = utl::hexDecode(text);
    if (key.size() != utl::kMlDsaPublicKeyBytes) {
      return BeaconServer::Error(BeaconServer::E_CONFIG,
                                 where + ".publicKeys entries must be hex ML-DSA-65 public keys");
    }
    out.publicKeys.push_back(std::move(key));
  }
  const uint64_t minSignatures = entry.getNonNegInt("minSignatures").value_or(out.publicKeys.size());
  if (minSignatures == 0 || minSignatures > out.publicKeys.size()) {
    return BeaconServer::Error(BeaconServer::E_CONFIG, where + ".minSignatures must be 1..number of keys");
  }
  out.minSignatures = static_cast<uint8_t>(minSignatures);
  return out;
}

} // namespace

// ============ InitFileConfig methods ============

Object BeaconServer::InitFileConfig::ltsToJson() {
  Object j;
  j.set("networkId", networkId);
  {
    // Placeholders: each holder creates its key pair (pp-client keygen -o <name>)
    // and fills in the public key; the beacon never sees their private keys.
    Object accounts;
    for (const char *name : {"genesis", "reserve", "registrar", "fee", "recycle"}) {
      Object entry;
      std::vector<Value> keys{Value(std::string("<hex public key from ") + name + ".pub>")};
      entry.set("publicKeys", Object::array(std::move(keys)));
      accounts.set(name, entry);
    }
    j.set("systemAccounts", accounts);
  }
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
  j.setJsonUInt("heartbeatSlots", heartbeatSlots);
  j.setJsonUInt("maxIssuancePerEpoch", maxIssuancePerEpoch);
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

  auto id = jd.getString("networkId");
  if (!id || id->empty()) {
    return Error(E_CONFIG, "Field 'networkId' (this chain's name, part of genesis) is required");
  }
  networkId = *id;

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

  if (jd.contains("heartbeatSlots")) {
    // Zero allowed: disables empty heartbeats.
    if (auto r = readU64("heartbeatSlots", heartbeatSlots, true, true); !r)
      return r;
  } else {
    // Default: at most ~one empty seal per idle epoch.
    heartbeatSlots = slotsPerEpoch;
  }

  if (auto r = readU64("maxIssuancePerEpoch", maxIssuancePerEpoch, false, true); !r) {
    return r;
  }

  if (auto accounts = parseSystemAccounts(jd); !accounts) {
    return accounts;
  }
  return parseGenesisMiners(jd);
}

BeaconServer::Roe<void> BeaconServer::InitFileConfig::parseSystemAccounts(const Object &jd) {
  const Object *accounts = jd.getObject("systemAccounts");
  if (!accounts) {
    return Error(E_CONFIG, "Field 'systemAccounts' {genesis, reserve, registrar, fee, recycle} with public keys is required");
  }
  const std::pair<const char *, Beacon::AccountKeys *> slots[] = {{"genesis", &systemAccounts.genesis},
                                                                  {"reserve", &systemAccounts.reserve},
                                                                  {"registrar", &systemAccounts.registrar},
                                                                  {"fee", &systemAccounts.fee},
                                                                  {"recycle", &systemAccounts.recycle}};
  for (const auto &[name, slot] : slots) {
    const Object *entry = accounts->getObject(name);
    if (!entry) {
      return Error(E_CONFIG, std::string("systemAccounts.") + name + " is required");
    }
    auto keys = parseAccountKeys(*entry, std::string("systemAccounts.") + name);
    if (!keys) {
      return keys.error();
    }
    *slot = std::move(keys.value());
  }
  return {};
}

BeaconServer::Roe<void> BeaconServer::InitFileConfig::parseGenesisMiners(const Object &jd) {
  genesisMiners.clear();
  if (!jd.contains("genesisMiners")) {
    return {};
  }
  const Array *list = jd.getArray("genesisMiners");
  if (!list) {
    return Error(E_CONFIG, "Field 'genesisMiners' must be an array");
  }
  size_t withoutStake = 0;
  for (size_t i = 0; i < list->elements.size(); ++i) {
    const std::string where = "genesisMiners[" + std::to_string(i) + "]";
    const Object *entry = asObject(list->elements[i]);
    if (!entry) {
      return Error(E_CONFIG, where + " must be an object");
    }
    Beacon::GenesisMiner miner;
    auto id = entry->getNonNegInt("id");
    if (!id) {
      return Error(E_CONFIG, where + ".id is required");
    }
    miner.id = *id;
    auto keys = parseAccountKeys(*entry, where);
    if (!keys) {
      return keys.error();
    }
    miner.publicKeys = std::move(keys.value().publicKeys);
    miner.minSignatures = keys.value().minSignatures;
    if (entry->contains("stake")) {
      auto stake = entry->getNonNegInt("stake");
      if (!stake || *stake == 0) {
        return Error(E_CONFIG, where + ".stake must be a positive integer");
      }
      miner.stake = *stake;
    } else {
      ++withoutStake;
    }
    genesisMiners.push_back(std::move(miner));
  }
  if (withoutStake > 0) {
    const uint64_t share = AccountBuffer::INITIAL_TOKEN_SUPPLY / GENESIS_MINER_STAKE_SHARE / withoutStake;
    for (auto &miner : genesisMiners) {
      if (miner.stake == 0) {
        miner.stake = share;
      }
    }
  }
  return {};
}

// ============ RunFileConfig methods ============

Object BeaconServer::RunFileConfig::ltsToJson() {
  Object j;
  j.setJsonUInt("port", port);
  return j;
}

BeaconServer::Roe<void>
BeaconServer::RunFileConfig::ltsFromJson(const Object &jd) {
  if (jd.contains("port")) {
    auto portValue = jd.getNonNegInt("port");
    if (!portValue || *portValue == 0 || *portValue > 65535) {
      return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
    }
    port = static_cast<uint16_t>(*portValue);
  }

  auto allowed = parseAllowedPeers(jd);
  if (!allowed) {
    return Error(E_CONFIG, allowed.error().message);
  }
  allowedPeers = std::move(allowed.value());
  return {};
}

// ============ BeaconServer methods ============

BeaconServer::BeaconServer() {
  redirectLogger("BeaconServer");
  beacon_.redirectLogger(log().getFullName() + ".Beacon");
  client_.redirectLogger(log().getFullName() + ".Client");
}

BeaconServer::Roe<void>
BeaconServer::init(const std::string &workDir, const std::vector<std::string> &genesisKeyFiles) {
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

    return Error("Created " + initConfigPath.string() +
                 ": set networkId and the system accounts' public keys (pp-client keygen -o <name>), then run "
                 "--init again with --genesis-key");
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
  log().info << "  Heartbeat slots: " << initFileConfig.heartbeatSlots;
  log().info << "  Max issuance per epoch: " << initFileConfig.maxIssuancePerEpoch;

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
  initConfig.chain.heartbeatSlots = initFileConfig.heartbeatSlots;
  initConfig.chain.maxIssuancePerEpoch = initFileConfig.maxIssuancePerEpoch;
  initConfig.chain.networkId = initFileConfig.networkId;
  initConfig.miners = initFileConfig.genesisMiners;
  log().info << "  Genesis miners: " << initConfig.miners.size();

  initConfig.key = initFileConfig.systemAccounts;
  for (const auto &keyFile : genesisKeyFiles) {
    auto privateKey = utl::readPrivateKey(keyFile, ".");
    if (!privateKey) {
      return Error("Failed to read genesis key '" + keyFile + "': " + privateKey.error().message);
    }
    initConfig.key.genesisSigners.push_back(privateKey.value());
  }

  auto result = initFromWorkDir(initConfig);
  if (!result) {
    return Error("Failed to initialize beacon: " + result.error().message);
  }


  log().info << "Beacon initialized successfully";
  return {};
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
  config_.network.udp_port = runFileConfig.port;

  log().info << "Configuration loaded";
  log().info << "  UDP port: " << config_.network.udp_port;

  // Initialize beacon core with mount config
  Beacon::MountConfig mountConfig;
  mountConfig.workDir = getWorkDir() + "/" + DIR_DATA;

  auto beaconMount = beacon_.mount(mountConfig);
  if (!beaconMount) {
    return Service::Error(-4, "Failed to mount Beacon: " +
                                  beaconMount.error().message);
  }

  log().info << "Beacon core initialized";

  auto keyResult = loadOrCreateIdentityKey();
  if (!keyResult) {
    return Service::Error(E_CONFIG, keyResult.error().message);
  }

  auto ampCfg = network::LedgerAmpConfigFromPrivateKey(*keyResult, config_.network.udp_port);
  if (!ampCfg) {
    return Service::Error(E_NETWORK, "Failed to build AMP config: " + ampCfg.error().message);
  }

  setAllowedPeers(runFileConfig.allowedPeers);
  auto serverStarted = startAmpServer(*ampCfg);
  if (!serverStarted) {
    return Service::Error(-5, "Failed to start AMP server: " + serverStarted.error().message);
  }

  initHandlers();
  return {};
}

void BeaconServer::onStop() {
  Server::onStop();
  log().info << "BeaconServer resources cleaned up";
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

  requestHandlers_[Client::T_REQ_TX_ADD] = [this](const Client::Request &request) { return hTxAdd(request); };
  requestHandlers_[Client::T_REQ_TX_PULL] = [this](const Client::Request &request) { return hTxPull(request); };
};


namespace {
int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

void BeaconServer::registerServer(const Client::MinerInfo &minerInfo) {
  miners_.upsert(minerInfo, nowSeconds());
  log().debug << "Miner record: " << minerInfo.id;
  expireMinerRecords();
}

void BeaconServer::expireMinerRecords() {
  const int64_t ttl = std::chrono::duration_cast<std::chrono::seconds>(MINER_RECORD_TTL).count();
  if (const size_t removed = miners_.expire(nowSeconds(), ttl); removed > 0) {
    log().info << "Dropped " << removed << " miner record(s) not renewed in " << ttl << " s";
  }
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
  state.networkId = beacon_.getNetworkId(); // from genesis
  state.registryVersion = miners_.version();
  if (state.nextBlockId > 0) {
    if (auto tip = beacon_.readBlock(state.nextBlockId - 1)) {
      state.headHash = tip.value().hash;
    }
  }

  return state;
}

void BeaconServer::runLoop() {
  log().info << "Request handler thread started";

  while (!isStopSet()) {
    try {
      beacon_.refresh();
      serveRequestsFor(std::chrono::milliseconds(100));
    } catch (const std::exception& e) {
      log().error << "Exception in request handler loop: " << e.what();
      serveRequestsFor(std::chrono::seconds(1));
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
  switch (checkBlockAdd(beacon_, block)) {
  case BlockAddCheck::AlreadyHave:
    return {"Block already added"};
  case BlockAddCheck::Conflicts:
    return Error(E_REQUEST, "Block " + std::to_string(block.block.index) + " conflicts with the stored block");
  case BlockAddCheck::New:
    break;
  }
  auto result = beacon_.addBlock(block);
  if (!result) {
    return Error(E_REQUEST, "Failed to add block: " + result.error().message);
  }
  txPool_.removeIncluded(block.block.records);
  return {"Block added"};
}

BeaconServer::Roe<std::string>
BeaconServer::hTxAdd(const Client::Request &request) {
  auto record = utl::binaryUnpack<Ledger::Record>(request.payload);
  if (!record) {
    return Error(E_REQUEST, "Failed to deserialize transaction: " + record.error().message);
  }
  if (!record.value().decode()) {
    return Error(E_REQUEST, "Unknown or malformed transaction payload");
  }
  const int64_t now = nowSeconds();
  txPool_.expire(now);
  switch (txPool_.add(record.value(), now)) {
  case TxPool::Add::Added:
    return {"Transaction submitted"};
  case TxPool::Add::Duplicate:
    return {"Transaction already submitted"};
  case TxPool::Add::Full:
    return Error(E_REQUEST, "Transaction pool full, please retry later");
  }
  return Error(E_REQUEST, "Transaction not accepted");
}

BeaconServer::Roe<std::string>
BeaconServer::hTxPull(const Client::Request & /*request*/) {
  txPool_.expire(nowSeconds());
  return utl::binaryPack(txPool_.pending(TX_PULL_MAX_RECORDS));
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
  if (AccountIds::isSystemAccount(minerInfo.id)) {
    // System accounts hold protocol funds and never lead slots.
    return Error(E_REQUEST, "System account " + std::to_string(minerInfo.id) + " cannot register as a miner");
  }
  // Cheap checks first; signature verification (ML-DSA) last.
  const int64_t skew = std::chrono::duration_cast<std::chrono::seconds>(REGISTER_MAX_SKEW).count();
  if (std::llabs(nowSeconds() - minerInfo.issuedAt) > skew) {
    return Error(E_REQUEST, "Registration timestamp is too far from the beacon's clock");
  }
  if (!miners_.isNewer(minerInfo)) {
    return Error(E_REQUEST, "Registration is not newer than the recorded one");
  }
  if (auto verified = beacon_.verifyMinerRegistration(minerInfo, buildStateResponse().networkId);
      !verified) {
    return Error(E_REQUEST, verified.error().message);
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
  expireMinerRecords();
  std::vector<pp::common::Meta> list;
  list.reserve(miners_.miners().size());
  for (const auto &[id, info] : miners_.miners()) {
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
