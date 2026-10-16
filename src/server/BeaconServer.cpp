#include "BeaconServer.h"
#include "../chain/ConfigPolicy.h"
#include "BlockAddPolicy.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/FileSync.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
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

/**
 * The beacon signing key pair under `workDir` (beacon-authority B006): both files must exist and
 * belong together.
 */
BeaconServer::Roe<utl::MlDsaKeyPair> loadBeaconSigningKey(const std::string &workDir, const std::string &keyName,
                                                          const std::string &pubName) {
  const std::filesystem::path keyPath = std::filesystem::path(workDir) / keyName;
  const std::filesystem::path pubPath = std::filesystem::path(workDir) / pubName;
  if (!std::filesystem::exists(keyPath) || !std::filesystem::exists(pubPath)) {
    return BeaconServer::Error(BeaconServer::E_CONFIG, "Beacon signing key needs both " + keyName + " and " + pubName);
  }
  auto privateKey = utl::readPrivateKey(keyName, workDir);
  if (!privateKey) {
    return BeaconServer::Error(BeaconServer::E_CONFIG, "Failed to read beacon signing key: " + privateKey.error().message);
  }
  std::ifstream pubFile(pubPath);
  std::string pubHex((std::istreambuf_iterator<char>(pubFile)), std::istreambuf_iterator<char>());
  pubHex.erase(std::remove_if(pubHex.begin(), pubHex.end(), [](unsigned char c) { return std::isspace(c); }),
               pubHex.end());
  const std::string publicKey = utl::hexDecode(pubHex);
  if (publicKey.size() != utl::kMlDsaPublicKeyBytes) {
    return BeaconServer::Error(BeaconServer::E_CONFIG, pubPath.string() + " is not a hex ML-DSA-65 public key");
  }
  const std::string probe = "pp-ledger/beacon-signing-key-probe/v1";
  auto sig = utl::mlDsaSign(privateKey.value(), probe);
  if (!sig || !utl::mlDsaVerify(publicKey, probe, sig.value())) {
    return BeaconServer::Error(BeaconServer::E_CONFIG, keyName + " and " + pubName + " do not match");
  }
  utl::MlDsaKeyPair pair;
  pair.publicKey = publicKey;
  pair.privateKey = privateKey.value();
  return pair;
}

/**
 * Every start: the beacon signing key, and the next one when an operator staged it for a rotation
 * (beacon-authority ba5). The beacon signs with whichever the certificate in force names.
 */
BeaconServer::Roe<std::vector<utl::MlDsaKeyPair>> loadBeaconSigningKeys(const std::string &workDir) {
  std::vector<utl::MlDsaKeyPair> keys;
  auto current = loadBeaconSigningKey(workDir, BeaconServer::FILE_BEACON_SIGNING_KEY,
                                      BeaconServer::FILE_BEACON_SIGNING_PUB);
  if (!current) {
    return current.error();
  }
  keys.push_back(current.value());
  const auto root = std::filesystem::path(workDir);
  if (std::filesystem::exists(root / BeaconServer::FILE_BEACON_SIGNING_NEXT_KEY) ||
      std::filesystem::exists(root / BeaconServer::FILE_BEACON_SIGNING_NEXT_PUB)) {
    auto next = loadBeaconSigningKey(workDir, BeaconServer::FILE_BEACON_SIGNING_NEXT_KEY,
                                     BeaconServer::FILE_BEACON_SIGNING_NEXT_PUB);
    if (!next) {
      return next.error();
    }
    keys.push_back(next.value());
  }
  return keys;
}

/**
 * --init: the beacon signing key pair, created when neither file exists (an operator may
 * pre-generate them with `pp-client keygen -o keys/beacon-signing`). Returns the raw public key.
 */
BeaconServer::Roe<std::string> loadOrCreateBeaconSigningKey(const std::string &workDir) {
  const std::filesystem::path keyPath = std::filesystem::path(workDir) / BeaconServer::FILE_BEACON_SIGNING_KEY;
  const std::filesystem::path pubPath = std::filesystem::path(workDir) / BeaconServer::FILE_BEACON_SIGNING_PUB;
  if (!std::filesystem::exists(keyPath) && !std::filesystem::exists(pubPath)) {
    std::error_code ec;
    std::filesystem::create_directories(keyPath.parent_path(), ec);
    auto pair = utl::mlDsaGenerate();
    if (!pair) {
      return BeaconServer::Error(BeaconServer::E_CONFIG, "Failed to generate beacon signing key: " + pair.error().message);
    }
    if (auto w = utl::writeToNewFile(keyPath.string(), utl::hexEncode(pair->privateKey) + "\n"); !w) {
      return BeaconServer::Error(BeaconServer::E_CONFIG, "Failed to write " + keyPath.string() + ": " + w.error().message);
    }
    if (auto w = utl::writeToNewFile(pubPath.string(), utl::hexEncode(pair->publicKey) + "\n"); !w) {
      return BeaconServer::Error(BeaconServer::E_CONFIG, "Failed to write " + pubPath.string() + ": " + w.error().message);
    }
    return pair->publicKey;
  }
  auto pair = loadBeaconSigningKey(workDir, BeaconServer::FILE_BEACON_SIGNING_KEY, BeaconServer::FILE_BEACON_SIGNING_PUB);
  if (!pair) {
    return pair.error();
  }
  return pair.value().publicKey;
}

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
    // The recovery set: held apart from the admin (genesis) keys, used almost never.
    Object recoveryEntry;
    std::vector<Value> recoveryKeys{Value(std::string("<hex public key from recovery1.pub>"))};
    recoveryEntry.set("publicKeys", Object::array(std::move(recoveryKeys)));
    j.set("recovery", recoveryEntry);
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
  j.setJsonUInt("newAccountFee", newAccountFee);
  j.setJsonUInt("minerMinStake", minerMinStake);
  j.setJsonUInt("minerRegistrationFee", minerRegistrationFee);
  j.setJsonUInt("minerDueAfterSlots", minerDueAfterSlots);
  j.setJsonUInt("minerMaxMissedSlots", minerMaxMissedSlots);
  j.setJsonUInt("minerReregisterCooldownEpochs", minerReregisterCooldownEpochs);
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
  if (auto r = readU64("newAccountFee", newAccountFee, false, true); !r) {
    return r;
  }
  if (jd.contains("minerDueAfterSlots")) {
    // Zero allowed: miners are never due.
    if (auto r = readU64("minerDueAfterSlots", minerDueAfterSlots, true, true); !r)
      return r;
  } else {
    minerDueAfterSlots = (DEFAULT_MINER_DUE_SECONDS + slotDuration - 1) / slotDuration;
  }
  for (const auto &[name, field] : {std::pair<const char *, uint64_t *>{"minerMinStake", &minerMinStake},
                                    {"minerRegistrationFee", &minerRegistrationFee},
                                    {"minerMaxMissedSlots", &minerMaxMissedSlots},
                                    {"minerReregisterCooldownEpochs", &minerReregisterCooldownEpochs}}) {
    if (auto r = readU64(name, *field, false, true); !r) {
      return r;
    }
  }

  if (auto accounts = parseSystemAccounts(jd); !accounts) {
    return accounts;
  }
  if (auto recoveryParsed = parseRecovery(jd); !recoveryParsed) {
    return recoveryParsed;
  }
  return parseGenesisMiners(jd);
}

BeaconServer::Roe<void> BeaconServer::InitFileConfig::parseRecovery(const Object &jd) {
  const Object *entry = jd.getObject("recovery");
  if (!entry) {
    return Error(E_CONFIG, "Field 'recovery' {publicKeys, minSignatures, delayEpochs} is required: the key set "
                           "that can replace the admin (genesis) keys");
  }
  auto keys = parseAccountKeys(*entry, "recovery");
  if (!keys) {
    return keys.error();
  }
  recovery = keys.value();
  if (entry->contains("delayEpochs")) {
    auto delay = entry->getNonNegInt("delayEpochs");
    if (!delay || *delay == 0) {
      return Error(E_CONFIG, "recovery.delayEpochs must be a positive integer");
    }
    recoveryDelayEpochs = *delay;
  } else {
    const uint64_t epochSeconds = slotDuration * slotsPerEpoch;
    recoveryDelayEpochs = std::max<uint64_t>(1, (DEFAULT_RECOVERY_DELAY_SECONDS + epochSeconds - 1) / epochSeconds);
  }
  return {};
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
  if (allowedPeers) {
    std::vector<Value> peers(allowedPeers->begin(), allowedPeers->end());
    j.set("allowedPeers", Object::array(std::move(peers)));
  }
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
BeaconServer::init(const std::string &workDir, const std::vector<std::string> &genesisKeyFiles, bool force) {
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
  log().info << "  New account fee: " << initFileConfig.newAccountFee;

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
  initConfig.chain.newAccountFee = initFileConfig.newAccountFee;
  initConfig.chain.minerMinStake = initFileConfig.minerMinStake;
  initConfig.chain.minerRegistrationFee = initFileConfig.minerRegistrationFee;
  initConfig.chain.minerDueAfterSlots = initFileConfig.minerDueAfterSlots;
  initConfig.chain.minerMaxMissedSlots = initFileConfig.minerMaxMissedSlots;
  initConfig.chain.minerReregisterCooldownEpochs = initFileConfig.minerReregisterCooldownEpochs;
  initConfig.chain.networkId = initFileConfig.networkId;
  if (auto liveness = chain_tx::validateLivenessSettings(initConfig.chain); !liveness) {
    return Error("Invalid init config: " + liveness.error().message);
  }
  initConfig.miners = initFileConfig.genesisMiners;
  log().info << "  Genesis miners: " << initConfig.miners.size();

  auto signingKey = loadOrCreateBeaconSigningKey(workDir);
  if (!signingKey) {
    return signingKey.error();
  }
  initConfig.authority.recovery.publicKeys = initFileConfig.recovery.publicKeys;
  initConfig.authority.recovery.minSignatures = initFileConfig.recovery.minSignatures;
  initConfig.authority.recovery.delayEpochs = initFileConfig.recoveryDelayEpochs;
  initConfig.authority.beacon.generation = 1;
  initConfig.authority.beacon.publicKey = signingKey.value();
  initConfig.authority.beacon.cutoverHeight = 0;
  log().info << "  Recovery set: " << initConfig.authority.recovery.publicKeys.size() << " key(s), "
             << static_cast<int>(initConfig.authority.recovery.minSignatures) << " to sign, delay "
             << initConfig.authority.recovery.delayEpochs << " epoch(s)";
  log().info << "  Beacon signing key: " << FILE_BEACON_SIGNING_KEY << " (generation 1)";

  initConfig.key = initFileConfig.systemAccounts;
  for (const auto &keyFile : genesisKeyFiles) {
    auto privateKey = utl::readPrivateKey(keyFile, ".");
    if (!privateKey) {
      return Error("Failed to read genesis key '" + keyFile + "': " + privateKey.error().message);
    }
    initConfig.key.genesisSigners.push_back(privateKey.value());
  }

  auto result = initFromWorkDir(initConfig, force);
  if (!result) {
    return Error("Failed to initialize beacon: " + result.error().message);
  }


  log().info << "Beacon initialized successfully";
  return {};
}

BeaconServer::Roe<void>
BeaconServer::initFromWorkDir(const Beacon::InitConfig &config, bool force) {
  log().info << "Initializing BeaconServer";

  // A chain is there already: genesis again would destroy it. Only on request.
  if (std::filesystem::exists(config.workDir) && !std::filesystem::is_empty(config.workDir) && !force) {
    return Error("Refusing to initialize: " + config.workDir +
                 " already holds a chain. Remove it first, or pass --force to replace it");
  }
  if (std::filesystem::exists(config.workDir)) {
    log().info << "  Removing existing work directory: " << config.workDir;
    std::error_code ec;
    std::filesystem::remove_all(config.workDir, ec);
    if (ec) {
      return Error("Failed to remove existing work directory: " + ec.message());
    }
  }

  // Genesis in a beacon of its own: its files are closed once it goes, so a failed attempt can be
  // removed (a corrected --init then finds no chain; Windows cannot remove open files).
  Beacon::Roe<void> result;
  {
    Beacon genesis;
    genesis.redirectLogger(log().getFullName() + ".Beacon");
    result = genesis.init(config);
  }
  if (!result) {
    std::error_code ec;
    std::filesystem::remove_all(config.workDir, ec);
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
  std::vector<std::string> clockReferences;

  if (promote_) {
    auto promotion = readRelayConfig();
    if (!promotion) {
      return Service::Error(E_CONFIG, promotion.error().message);
    }
    runFileConfig = promotion.value().config;
    clockReferences = promotion.value().clockReferences;
  } else {
    auto loaded = env_config::loadConfig(configPathStr, env_config::commonFields());
    if (!loaded) {
      return Service::Error(E_CONFIG, loaded.error().message);
    }
    if (!loaded.value()) {
      // No file and no environment: the defaults (nothing is written; the directory may be read-only).
      log().info << "No " << FILE_CONFIG << " and no PP_LEDGER_* settings: using defaults";
    } else if (auto parseResult = runFileConfig.ltsFromJson(*loaded.value()); !parseResult) {
      return Service::Error(E_CONFIG, "Failed to parse config (" + configPathStr + " and PP_LEDGER_* variables): " +
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

  auto signingKeys = loadBeaconSigningKeys(getWorkDir());
  if (!signingKeys) {
    return Service::Error(E_CONFIG, signingKeys.error().message);
  }
  if (promote_) {
    if (auto promotable = checkPromotion(signingKeys.value()); !promotable) {
      return Service::Error(E_CONFIG, "Cannot promote: " + promotable.error().message);
    }
  }
  if (auto signing = beacon_.openTipSigning(signingKeys.value()); !signing) {
    return Service::Error(-4, "Failed to open tip signing: " + signing.error().message);
  }
  if (const auto by = beacon_.supersededBy()) {
    log().error << "Beacon generation " << *by << " certifies none of this beacon's keys: it serves reads only";
  } else if (const auto generation = beacon_.takingOver()) {
    log().warning << "Taking over as beacon generation " << *generation << " at block "
                  << beacon_.getHeldCertificate()->cutoverHeight + 1;
  }

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

  if (promote_) {
    client_.attachAmpTransport(*ampRuntime(), "clock:0");
    auto references = clockReferences;
    for (const auto &hint : beacon_.getDialHints()) {
      references.push_back(hint);
    }
    if (auto clock = checkClockForTakeover(std::move(references)); !clock) {
      return Service::Error(E_CONFIG, "Cannot promote: " + clock.error().message);
    }
    if (auto written = writeBeaconConfig(runFileConfig); !written) {
      return Service::Error(E_CONFIG, "Cannot promote: " + written.error().message);
    }
    log().warning << "Promoted: this relay's work directory now runs the beacon (its relay config is kept in "
                  << FILE_RELAY_CONFIG << ")";
  }

  initHandlers();
  return {};
}

BeaconServer::Roe<BeaconServer::Promotion> BeaconServer::readRelayConfig() const {
  const std::string path = (std::filesystem::path(getWorkDir()) / FILE_CONFIG).string();
  auto json = utl::loadJsonFile(path);
  if (!json) {
    return Error(E_CONFIG, "Failed to load " + path + ": " + json.error().message);
  }
  const Object &jd = json.value();
  if (!jd.contains("beacon") && !jd.contains("beacons")) {
    return Error(E_CONFIG, std::string(FILE_CONFIG) +
                               " is not a relay's (no `beacon` or `beacons`): --promote turns a relay's work "
                               "directory into the beacon");
  }
  Promotion promotion;
  if (auto parsed = promotion.config.ltsFromJson(jd); !parsed) {
    return parsed.error();
  }
  if (!jd.contains("port")) {
    promotion.config.port = Client::DEFAULT_RELAY_PORT; // the port the relay listened on
  }
  if (auto beacon = jd.getString("beacon")) {
    promotion.clockReferences.push_back(*beacon);
  }
  if (const Array *beacons = jd.getArray("beacons")) {
    for (const auto &element : beacons->elements) {
      if (auto value = asString(element)) {
        promotion.clockReferences.push_back(*value);
      }
    }
  }
  return promotion;
}

BeaconServer::Roe<void> BeaconServer::checkPromotion(const std::vector<utl::MlDsaKeyPair> &keys) const {
  if (std::filesystem::exists(std::filesystem::path(getWorkDir()) / DIR_DATA / Beacon::FILE_SIGNED_TIP)) {
    return Error(E_CONFIG, std::string(DIR_DATA) + "/" + Beacon::FILE_SIGNED_TIP +
                               " exists: this directory was a beacon's, and --promote takes a relay's");
  }
  const auto held = beacon_.getHeldCertificate();
  if (!held) {
    return Error(E_CONFIG, "no beacon certificate above generation " + std::to_string(beacon_.getGeneration()) +
                               " is held here: hand this relay the admins' certificate (pp-client -b --host "
                               "<relay> submit-tx <file>) and promote again");
  }
  const std::string generation = "the certificate held (beacon generation " + std::to_string(held->generation) + ")";
  if (!beacon_.takeoverWith(keys)) {
    const bool namesOurs = std::any_of(keys.begin(), keys.end(), [&held](const utl::MlDsaKeyPair &key) {
      return key.publicKey == held->publicKey;
    });
    return Error(E_CONFIG, namesOurs ? std::string("this directory holds the key in force: it is a beacon's already")
                                     : generation + " names another key than " + FILE_BEACON_SIGNING_PUB);
  }
  if (!held->hasCutover()) {
    return Error(E_CONFIG, generation + " has no cutover: a beacon takes over at one (mk-beacon-cert --cutover)");
  }
  if (beacon_.cutoverForked()) {
    return Error(E_CONFIG, "block " + std::to_string(held->cutoverHeight) +
                               " here is not the one " + generation + " vouches for: promote another relay");
  }
  if (beacon_.getNextBlockId() <= held->cutoverHeight) {
    return Error(E_CONFIG, "the chain here ends at block " + std::to_string(beacon_.getNextBlockId() - 1) +
                               ", below the cutover (block " + std::to_string(held->cutoverHeight) +
                               "): run it as a relay until it holds that block, then promote");
  }
  return {};
}

BeaconServer::Roe<void> BeaconServer::checkClockForTakeover(std::vector<std::string> references) {
  std::vector<int64_t> offsets;
  std::set<std::string> asked;
  for (const auto &reference : references) {
    if (isSelf(reference) || !asked.insert(reference).second) {
      continue;
    }
    const std::string key = "clock:" + std::to_string(asked.size());
    auto estimated = calibrateBlocking(calibrationFetch(client_, key, reference));
    if (!estimated) {
      log().warning << "Clock check: " << reference << " did not answer: " << estimated.error().message;
      continue;
    }
    log().info << "Clock check: " << reference << " is " << estimated.value().offsetMs << " ms off this host";
    offsets.push_back(estimated.value().offsetMs);
  }
  if (offsets.empty()) {
    log().warning << "Clock check: no node answered; this host's clock is not checked against the network "
                  << "(keep it on NTP)";
    return {};
  }
  std::sort(offsets.begin(), offsets.end());
  const int64_t median = offsets[offsets.size() / 2];
  const int64_t bound = consensus::SlotCommittee::deliveryWindowMs(beacon_.getSlotDuration());
  if (std::llabs(median) > bound) {
    return Error(E_CONFIG, "this host's clock is " + std::to_string(median) + " ms off the network's (" +
                               std::to_string(offsets.size()) + " node(s) asked), beyond the delivery window (" +
                               std::to_string(bound) + " ms): set its time (NTP) and promote again");
  }
  log().info << "Clock check: " << median << " ms off the network (" << offsets.size() << " node(s); bound "
             << bound << " ms)";
  return {};
}

BeaconServer::Roe<void> BeaconServer::writeBeaconConfig(RunFileConfig config) const {
  const auto dir = std::filesystem::path(getWorkDir());
  std::ifstream in(dir / FILE_CONFIG, std::ios::binary);
  const std::string relayConfig((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) {
    return Error(E_CONFIG, "Failed to read " + std::string(FILE_CONFIG));
  }
  if (auto kept = utl::replaceFile((dir / FILE_RELAY_CONFIG).string(), relayConfig); !kept) {
    return Error(E_CONFIG, "Failed to keep the relay config: " + kept.error().message);
  }
  auto encoded = encodeObjectPretty(config.ltsToJson());
  if (!encoded) {
    return Error(E_CONFIG, "Failed to encode " + std::string(FILE_CONFIG) + ": " + encoded.error().message);
  }
  if (auto written = utl::replaceFile((dir / FILE_CONFIG).string(), encoded.value() + "\n"); !written) {
    return Error(E_CONFIG, "Failed to write " + std::string(FILE_CONFIG) + ": " + written.error().message);
  }
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

  requestHandlers_[Client::T_REQ_AUTHORITY_PROOF] = [this](const Client::Request &request) {
    return hAuthorityProof(request);
  };

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
  requestHandlers_[Client::T_REQ_EQUIVOCATION_ADD] = [this](const Client::Request &request) {
    return hEquivocationAdd(request);
  };
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
  state.signedTip = beacon_.getSignedTip();
  state.certificate = beacon_.getCurrentCertificate();
  state.equivocation = beacon_.getEquivocation();
  state.recovery = beacon_.getRecoveryStatus();
  state.rewound = beacon_.getLastRewind();
  if (state.nextBlockId > 0) {
    if (auto tip = beacon_.readBlock(state.nextBlockId - 1)) {
      state.headHash = tip.value().hash;
    }
  }

  return state;
}

void BeaconServer::orderRewindIfDue() {
  if (restartRequested()) {
    return;
  }
  if (auto due = beacon_.scheduleRewind(approvedRewindGeneration()); !due) {
    log().error << due.error().message;
  } else if (due.value()) {
    requestRestart(kRewindRestartGrace); // a forward in flight (the certificate itself) finishes
  }
}

void BeaconServer::runLoop() {
  log().info << "Request handler thread started";

  while (!isStopSet()) {
    try {
      orderRewindIfDue();
      beacon_.refresh();
      serveRequestsFor(std::chrono::milliseconds(100));
    } catch (const std::exception& e) {
      log().error << "Exception in request handler loop: " << e.what();
      serveRequestsFor(std::chrono::seconds(1));
    }
  }

  log().info << "Request handler thread stopped";
}

namespace {
/**
 * Requests that make the beacon act as the terminal: refused once it stood down. TX_ADD is checked
 * in its handler, which still takes a beacon certificate (ba5c).
 */
bool isTerminalWrite(uint32_t type) {
  return type == Client::T_REQ_BLOCK_ADD || type == Client::T_REQ_TX_PULL || type == Client::T_REQ_REGISTER;
}
} // namespace

std::string BeaconServer::handleParsedRequest(const Client::Request &request) {
  log().debug << "Handling request: " << request.type;
  if (const auto by = isTerminalWrite(request.type) ? beacon_.supersededBy() : std::nullopt) {
    return Server::packResponse(1, "This beacon stood down: beacon generation " + std::to_string(*by) +
                                       " certifies another key");
  }
  if (const auto above = isTerminalWrite(request.type) ? beacon_.haltedAbove() : std::nullopt) {
    return Server::packResponse(1, haltedMessage(*above));
  }
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
    return tipReply(); // a leader retrying: the signed tip covers its block
  case BlockAddCheck::Conflicts:
    return Error(E_REQUEST, "Block " + std::to_string(block.block.index) + " conflicts with the stored block");
  case BlockAddCheck::New:
    break;
  }
  // The beacon decides missed slots: a leader seals once its slot ends and must deliver within
  // the delivery window. Checked on live admission only (the beacon's clock), never on replay
  // or by relays, so every node follows the beacon's chain.
  if (!beacon_.isBeforeDeliveryDeadline(block.block.slot)) {
    return Error(E_REQUEST, "Block for slot " + std::to_string(block.block.slot) +
                                " arrived after its delivery deadline");
  }
  auto result = beacon_.addBlock(block);
  if (!result) {
    return Error(E_REQUEST, "Failed to add block: " + result.error().message);
  }
  txPool_.removeIncluded(block.block.records);
  return tipReply();
}

std::string BeaconServer::haltedMessage(uint64_t above) const {
  return "This beacon is halted: beacon generation " + std::to_string(beacon_.getEquivocation()->generation()) +
         " signed two blocks at height " + std::to_string(above) +
         " (equivocation); nothing above it is committed until a later generation is certified";
}

std::string BeaconServer::tipReply() const {
  Client::TipReply reply;
  reply.nextBlockId = beacon_.getNextBlockId();
  reply.setTip(beacon_.getSignedTip());
  return utl::binaryPack(reply);
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
  const bool certificate = record.value().type == Ledger::T_BEACON_CERT;
  if (certificate) {
    if (auto kept = offerCertificate(record.value()); !kept) {
      return kept.error();
    }
  }
  if (const auto by = beacon_.supersededBy()) {
    const std::string why = "beacon generation " + std::to_string(*by) + " certifies another key";
    if (certificate) {
      return {"Beacon certificate kept; this beacon stood down: " + why};
    }
    return Error(E_REQUEST, "This beacon stood down: " + why);
  }
  // Halted (ba6): only a certificate — the way out — is still taken.
  if (const auto above = beacon_.haltedAbove(); above && !certificate) {
    return Error(E_REQUEST, haltedMessage(*above));
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

BeaconServer::Roe<std::string> BeaconServer::hEquivocationAdd(const Client::Request &request) {
  Client::Equivocation evidence;
  if (!evidence.ltsFromString(request.payload)) {
    return Error(E_REQUEST, "Invalid equivocation evidence");
  }
  auto offered = beacon_.offerEquivocation(evidence);
  if (!offered) {
    return Error(E_REQUEST, "Equivocation evidence refused: " + offered.error().message);
  }
  switch (offered.value()) {
  case Chain::EquivocationOffer::Newer:
  case Chain::EquivocationOffer::Same:
    return {"Equivocation evidence kept: halted above block " + std::to_string(evidence.height())};
  case Chain::EquivocationOffer::Ignored:
    return {"Equivocation evidence ignored: its generation is superseded, or evidence is already held"};
  }
  return {"Equivocation evidence ignored"};
}

BeaconServer::Roe<void> BeaconServer::offerCertificate(const Ledger::Record &record) {
  auto offered = beacon_.offerCertificate(record);
  if (!offered) {
    return Error(E_REQUEST, "Beacon certificate refused: " + offered.error().message);
  }
  switch (offered.value()) {
  case Chain::CertificateOffer::Newer:
  case Chain::CertificateOffer::Same:
    return {};
  case Chain::CertificateOffer::Older:
    return Error(E_REQUEST, "Beacon certificate refused: its generation is not above the one in force or held");
  case Chain::CertificateOffer::Conflict:
    return Error(E_REQUEST, "Beacon certificate refused: another certificate for its generation is held");
  }
  return {};
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

BeaconServer::Roe<std::string> BeaconServer::hAuthorityProof(const Client::Request &request) {
  auto upTo = utl::binaryUnpack<uint64_t>(request.payload);
  if (!upTo) {
    return Error(E_REQUEST, "Invalid authority proof payload");
  }
  auto proof = beacon_.authorityProof(upTo.value());
  if (!proof) {
    return Error(E_REQUEST, "Failed to build authority proof: " + proof.error().message);
  }
  return utl::binaryPack(proof.value());
}

} // namespace pp
