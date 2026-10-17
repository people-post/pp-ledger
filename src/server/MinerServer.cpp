#include "MinerServer.h"
#include "../chain/PredecessorCheck.h"
#include "../chain/BlockValidation.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "../network/amp/AmpIdentity.h"
#include "NetworkAnchor.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "common/io/Json.h"
#include <vector>

namespace pp {
namespace {
using pp::common::Array;
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


Object MinerServer::RunFileConfig::ltsToJson() const {
  Object j;
  j.setUIntForJson("minerId", minerId);
  std::vector<Value> keyVals;
  for (const auto &k : keys) {
    keyVals.push_back(k);
  }
  if (keyVals.empty()) {
    keyVals.push_back(std::string("<private key file: pp-client keygen -o key>"));
  }
  j.set("keys", Object::array(std::move(keyVals)));
  j.setJsonUInt("port", port);
  std::vector<Value> beaconVals;
  for (const auto &b : beacons) {
    beaconVals.push_back(b);
  }
  if (beaconVals.empty()) {
    beaconVals.push_back(std::string("<upstream multiaddr: /ip4/A.B.C.D/udp/8519/adp/1.0.0/p2p/PEER_ID>"));
  }
  j.set("beacons", Object::array(std::move(beaconVals)));
  return j;
}

namespace {
/** config.json fields a miner takes from the environment (CONFIGURATION.md § Environment). */
std::vector<env_config::Field> minerEnvFields() {
  auto fields = env_config::commonFields();
  fields.push_back({"PP_MINER_ID", "minerId", env_config::Kind::UInt, "", false});
  fields.push_back({"PP_MINER_KEYS", "keys", env_config::Kind::List, "", /*secret=*/true});
  fields.push_back({"PP_LEDGER_UPSTREAMS", "beacons", env_config::Kind::List, "", false});
  for (auto &field : env_config::anchorFields()) {
    fields.push_back(std::move(field));
  }
  return fields;
}
} // namespace

MinerServer::Roe<void>
MinerServer::RunFileConfig::ltsFromJson(const Object &jd) {
  auto minerIdOpt = jd.getNonNegInt("minerId");
  if (!minerIdOpt) {
    return Error(E_CONFIG, jd.contains("minerId")
                               ? "Field 'minerId' must be a non-negative integer"
                               : "Field 'minerId' is required");
  }
  minerId = *minerIdOpt;
  if (AccountIds::isSystemAccount(minerId)) {
    return Error(E_CONFIG, "Field 'minerId' must be this miner's account id (system accounts, ids below " +
                               std::to_string(AccountIds::ID_FIRST_ISSUED) + ", never mine)");
  }

  const Array *keysArr = jd.getArray("keys");
  if (!keysArr) {
    return Error(E_CONFIG, jd.contains("keys") ? "Field 'keys' must be an array"
                                               : "Field 'keys' is required");
  }
  keys.clear();
  for (size_t i = 0; i < keysArr->elements.size(); ++i) {
    auto keyFile = asString(keysArr->elements[i]);
    if (!keyFile) {
      return Error(E_CONFIG,
                   "All elements in 'keys' array must be strings (index " +
                       std::to_string(i) + " is not)");
    }
    if (keyFile->empty()) {
      return Error(E_CONFIG, "Key file at index " + std::to_string(i) +
                                 " cannot be empty");
    }
    keys.push_back(*keyFile);
  }
  if (keys.empty()) {
    return Error(E_CONFIG, "Field 'keys' array must contain at least one key file");
  }

  if (jd.contains("port")) {
    auto portValue = jd.getNonNegInt("port");
    if (!portValue || *portValue == 0 || *portValue > 65535) {
      return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
    }
    port = static_cast<uint16_t>(*portValue);
  }

  const Array *beaconsArr = jd.getArray("beacons");
  if (!beaconsArr) {
    return Error(E_CONFIG, jd.contains("beacons") ? "Field 'beacons' must be an array"
                                                  : "Field 'beacons' is required");
  }
  if (beaconsArr->elements.empty()) {
    return Error(E_CONFIG, "Field 'beacons' array must contain at least one beacon entry");
  }
  beacons.clear();
  for (size_t i = 0; i < beaconsArr->elements.size(); ++i) {
    auto s = asString(beaconsArr->elements[i]);
    if (!s) {
      return Error(E_CONFIG, "beacons[" + std::to_string(i) + "] must be an upstream multiaddr string");
    }
    auto ma = network::ParseBeaconMultiaddrString(*s);
    if (!ma) {
      return Error(E_CONFIG, "beacons[" + std::to_string(i) + "]: " + ma.error().message);
    }
    beacons.push_back(std::move(*ma));
  }
  if (const Object* anchorObj = jd.getObject("networkAnchor")) {
    auto anchor = NetworkAnchor::fromJson(*anchorObj);
    if (!anchor) {
      return Error(E_CONFIG, anchor.error().message);
    }
    network_anchor = anchor.value();
  }
  auto allowed = parseAllowedPeers(jd);
  if (!allowed) {
    return Error(E_CONFIG, allowed.error().message);
  }
  allowedPeers = std::move(allowed.value());
  return {};
}

// ============ MinerServer methods ============

MinerServer::MinerServer() {
  bindCore(miner_);
  redirectLogger("MinerServer");
  miner_.redirectLogger(log().getFullName() + ".Miner");
  client_.redirectLogger(log().getFullName() + ".Client");
  syncClient_.redirectLogger(log().getFullName() + ".SyncClient");
  fanoutClient_.redirectLogger(log().getFullName() + ".FanoutClient");
}

MinerServer::~MinerServer() {}

MinerServer::Roe<void> MinerServer::dialUpstream(Client &client, size_t index) {
  if (!upstreams_ || index >= upstreams_->all().size()) {
    return Error(E_CONFIG, "No upstream [" + std::to_string(index) + "]");
  }
  if (auto dial = client.setAmpPeer(UpstreamSet::dialKey(index), upstreams_->all()[index]); !dial) {
    return Error(E_NETWORK, dial.error().message);
  }
  return {};
}

MinerServer::Roe<void> MinerServer::dialActiveUpstream() {
  return dialUpstream(client_, upstreams_ ? upstreams_->activeIndex() : 0);
}

void MinerServer::initUpstreams() {
  UpstreamSet::Hooks hooks = upstreamHooks(fanoutClient_);
  hooks.admit = [this](size_t index, const Client::BeaconState &state) {
    if (auto verified = verifyUpstreamState(state); !verified) {
      log().warning << "Disregarding upstream [" << index << "]: " << verified.error().message;
      return false;
    }
    return true;
  };
  hooks.learn = [this](size_t index, const Client::BeaconState &state) {
    const std::string from = "upstream [" + std::to_string(index) + "]'s STATUS";
    if (state.certificate) {
      noteCertificate(*state.certificate, from);
    }
    if (state.equivocation) {
      miner_.noteEquivocation(*state.equivocation, from);
    }
  };
  hooks.offerEquivocation = [this](const Client::Equivocation &evidence, const std::string &from) {
    if (miner_.noteEquivocation(evidence, from) == Chain::EquivocationOffer::Newer) {
      pushEquivocationUp(evidence);
    }
  };
  hooks.mayFollow = [this](const Client::SignedTip &tip) { return miner_.mayFollowTip(tip); };
  upstreams_ = std::make_unique<UpstreamSet>(config_.network.beacon_multiaddrs, std::move(hooks));
  upstreams_->redirectLogger(log().getFullName() + ".Upstreams");
}

std::optional<Chain::CertificateOffer> MinerServer::noteCertificate(const Ledger::Record &record,
                                                                    const std::string &from) {
  auto offered = miner_.noteCertificate(record, from);
  if (offered == Chain::CertificateOffer::Newer && upstreams_) {
    // bp2b: up the tree, to the new beacon's gateways, and follow whichever serves the new generation.
    pushCertificateUp(record);
    upstreams_->addHints(miner_.getDialHints());
    upstreams_->rerank("a new beacon certificate");
  }
  return offered;
}

void MinerServer::pushCertificateUp(const Ledger::Record &record) {
  for (size_t i = 0; i < upstreams_->all().size(); ++i) {
    if (auto dial = dialUpstream(fanoutClient_, i); !dial) {
      log().warning << "Certificate not passed to upstream [" << i << "]: " << dial.error().message;
      continue;
    }
    fanoutClient_.addTransactionAsync(record, completeOnServerThread<void>([this, i](Client::Roe<void> result) {
      if (!result) {
        log().debug << "Upstream [" << i << "] did not take the certificate: " << result.error().message;
      }
    }));
  }
}

MinerServer::Roe<void> MinerServer::verifyUpstreamState(const Client::BeaconState& state) {
  return NetworkAnchor::verifyStatus<Error>(config_.network.network_anchor, state);
}

MinerServer::Roe<void> MinerServer::verifyGenesisAnchor() {
  const auto &anchor = config_.network.network_anchor;
  if (anchor.genesis_hash.empty() && anchor.predecessor_dir.empty()) {
    return {};
  }
  auto block = client_.fetchBlock(0);
  if (!block) {
    return Error(E_NETWORK,
                 "Failed to fetch genesis block for anchor verification: " +
                     block.error().message);
  }
  // Recompute the hash from the block body instead of trusting the upstream
  // peer's self-reported `hash` field, which a malicious/compromised
  // upstream could set to match the pinned anchor while serving a forged
  // genesis body.
  const std::string calculatedHash =
      chain_block::calculateBlockHash(block.value().block);
  // A miner starting at a checkpoint never syncs block 0: it checks the predecessor here (B037).
  if (!anchor.predecessor_dir.empty()) {
    if (auto checked = predecessor_check::check(block.value(), anchor.predecessor_dir); !checked) {
      return Error(E_CONFIG, "Genesis refused: " + checked.error().message);
    }
    log().info << "Re-genesis checked against the old ledger in " << anchor.predecessor_dir;
  }
  if (anchor.genesis_hash.empty()) {
    return {}; // checked, but not pinned: the first block is still taken as the upstream serves it
  }
  if (auto pinned = NetworkAnchor::verifyGenesisHash<Error>(config_.network.network_anchor, calculatedHash);
      !pinned) {
    return pinned;
  }
  anchoredGenesis_ = block.value();
  anchoredGenesis_->hash = calculatedHash;
  return {};
}

MinerServer::Roe<uint64_t> MinerServer::chooseStartingBlock(const Client::BeaconState &state) {
  pinnedFirstBlockHash_.reset();
  anchoredStart_.reset();
  if (!anchoredGenesis_) {
    return state.checkpointId; // no anchor: the first block is taken on trust
  }
  if (state.checkpointId == 0) {
    pinnedFirstBlockHash_ = anchoredGenesis_->hash;
    return 0;
  }
  // The checkpoint carries the authority and admin set a node starting there learns; replaying the
  // admin-signed authority changes from the anchored genesis must reach the same (beacon-authority
  // ba5b). The sync checks the block it actually starts from again (checkStartingAuthority).
  auto replayed = replayAuthorityTo(state.checkpointId);
  if (replayed) {
    log().info << "Starting at checkpoint " << state.checkpointId
               << ": its authority proof ties it to the anchored genesis";
    anchoredStart_ = AnchoredStart{state.checkpointId, std::move(replayed.value())};
    return state.checkpointId;
  }
  log().warning << "Cannot tie checkpoint " << state.checkpointId << " to the anchored genesis ("
                << replayed.error().message << "): syncing from genesis";
  pinnedFirstBlockHash_ = anchoredGenesis_->hash;
  return 0;
}

MinerServer::Roe<AuthorityTrail> MinerServer::replayAuthorityTo(uint64_t checkpointId) {
  auto proof = client_.fetchAuthorityProof(checkpointId);
  if (!proof) {
    return Error(E_NETWORK, "Failed to fetch the authority proof: " + proof.error().message);
  }
  auto checkpoint = client_.fetchBlock(checkpointId);
  if (!checkpoint) {
    return Error(E_NETWORK, "Failed to fetch the checkpoint block: " + checkpoint.error().message);
  }
  // The checkpoint's epoch: a recovery due by then has replaced the admin set it carries (ba7).
  auto replayed = AuthorityTrail::replayProof(anchoredGenesis_->block, proof.value(), checkpointId,
                                              checkpoint.value().block.epoch);
  if (!replayed) {
    return Error(E_NETWORK, "The authority proof does not replay: " + replayed.error().message);
  }
  auto carried = AuthorityTrail::fromStartingBlock(checkpoint.value().block);
  if (!carried) {
    return Error(E_NETWORK, carried.error().message);
  }
  if (auto same = replayed.value().agrees(carried.value().authority(), carried.value().admin()); !same) {
    return Error(E_NETWORK, same.error().message);
  }
  return replayed.value();
}

pp::Roe<void> MinerServer::checkStartingAuthority(uint64_t blockId, const ChainAuthority &authority,
                                                  const Client::Wallet &admin) const {
  if (!anchoredStart_) {
    return {}; // no anchor (taken on trust), or genesis (pinned by hash)
  }
  if (blockId != anchoredStart_->blockId) {
    return pp::Error("starts at block " + std::to_string(blockId) + ", not the checkpoint the proof covers");
  }
  auto same = anchoredStart_->trail.agrees(authority, admin);
  return same ? pp::Roe<void>() : pp::Roe<void>(pp::Error(same.error().message));
}

void MinerServer::pushEquivocationUp(const Client::Equivocation &evidence) {
  // To every upstream: each side of a split runs its own beacon host, and each must halt.
  for (size_t i = 0; i < upstreams_->all().size(); ++i) {
    if (auto dial = dialUpstream(fanoutClient_, i); !dial) {
      log().warning << "Equivocation evidence not passed to upstream [" << i << "]: " << dial.error().message;
      continue;
    }
    fanoutClient_.addEquivocationAsync(evidence, completeOnServerThread<std::string>([this, i](Client::Roe<std::string> result) {
      if (!result) {
        log().warning << "Equivocation evidence not passed to upstream [" << i << "]: " << result.error().message;
      }
    }));
  }
}

void MinerServer::dEquivocationAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  Client::Equivocation evidence;
  if (!evidence.ltsFromString(request.payload)) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid equivocation evidence")));
    return;
  }
  const auto offered = miner_.noteEquivocation(evidence, "an EQUIVOCATION_ADD");
  const std::string kept = offered == Chain::EquivocationOffer::Newer || offered == Chain::EquivocationOffer::Same
                               ? "Equivocation evidence kept"
                               : "Equivocation evidence not kept here";
  if (auto dial = dialActiveUpstream(); !dial) {
    replyWith(reply, Roe<std::string>(kept + "; not forwarded: " + dial.error().message));
    return;
  }
  client_.addEquivocationAsync(evidence, completeOnServerThread<std::string>([reply, kept](Client::Roe<std::string> result) {
    replyWith(reply, Roe<std::string>(kept + "; upstream: " + (result ? result.value() : result.error().message)));
  }));
}

Service::Roe<void> MinerServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(getWorkDir()) / FILE_CONFIG;
  std::string configPathStr = configPath.string();

  // Create default FILE_CONFIG if it doesn't exist using RunFileConfig
  RunFileConfig runFileConfig;

  auto loaded = env_config::loadConfig(configPathStr, minerEnvFields());
  if (!loaded) {
    return Service::Error(E_CONFIG, loaded.error().message);
  }
  if (!loaded.value()) {
    log().info << "No " << FILE_CONFIG
               << " found, creating with default values";

    // Use default values from RunFileConfig struct
    auto encoded = encodeObjectPretty(runFileConfig.ltsToJson());
    if (!encoded) {
      return Service::Error(E_ROLE, "Failed to encode " + std::string(FILE_CONFIG) +
                                         ": " + encoded.error().message);
    }
    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_ROLE,
                            "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << encoded.value() << std::endl;
    configFile.close();

    // Nothing here can be guessed (account id, keys, upstream): stop until set.
    return Service::Error(E_CONFIG, "Created " + configPathStr +
                                        ": set minerId, keys and beacons (upstream multiaddrs), then start again — or "
                                        "set PP_MINER_ID, PP_MINER_KEYS (or PP_MINER_KEYS_FILE) and PP_LEDGER_UPSTREAMS");
  } else if (auto parseResult = runFileConfig.ltsFromJson(*loaded.value()); !parseResult) {
    return Service::Error(E_CONFIG, "Failed to parse config (" + configPathStr + " and PP_LEDGER_* variables): " +
                                        parseResult.error().message);
  }

  // Apply configuration from RunFileConfig
  config_.minerId = runFileConfig.minerId;
  std::filesystem::path configDir =
      std::filesystem::path(getWorkDir());
  config_.privateKeys.clear();
  for (const auto &keyFile : runFileConfig.keys) {
    auto keyResult = utl::readPrivateKey(keyFile, configDir.string());
    if (!keyResult) {
      return Service::Error(E_CONFIG,
                            "Failed to load key '" + keyFile + "': " +
                                keyResult.error().message);
    }
    config_.privateKeys.push_back(keyResult.value());
  }
  config_.network.udp_port = runFileConfig.port;
  config_.network.beacon_multiaddrs = runFileConfig.beacons;
  config_.network.network_anchor = runFileConfig.network_anchor;

  log().info << "Configuration loaded";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  UDP port: " << config_.network.udp_port;
  log().info << "  Beacons: " << config_.network.beacon_multiaddrs.size();

  if (auto upstreams = setUpstreams(config_.network.beacon_multiaddrs); !upstreams) {
    return Service::Error(E_CONFIG, upstreams.error().message);
  }
  initUpstreams();

  auto ampCfg = network::LedgerAmpConfigFromPrivateKey(config_.privateKeys.front(), config_.network.udp_port);
  if (!ampCfg) {
    return Service::Error(E_CONFIG, "Failed to build AMP config: " + ampCfg.error().message);
  }
  setAllowedPeers(runFileConfig.allowedPeers);
  auto serverStarted = startAmpServer(*ampCfg);
  if (!serverStarted) {
    return Service::Error(E_ROLE, "Failed to start AMP server: " + serverStarted.error().message);
  }

  if (!ampRuntime()) {
    return Service::Error(E_NETWORK, "AMP runtime unavailable after start");
  }
  client_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));
  syncClient_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));
  fanoutClient_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));

  // Connect to beacon server and fetch initial state
  auto beaconResult = connectToBeacon();
  if (!beaconResult && isStopSet()) {
    return {}; // a stop request ended the wait: Service::run does not run
  }
  if (!beaconResult) {
    return Service::Error(E_NETWORK, "Failed to connect to beacon: " +
                                         beaconResult.error().message);
  }
  log().info
      << "Successfully connected to beacon and synchronized initial state";
  const auto &state = beaconResult.value();

  auto calResult = calibrateTimeToBeacon();
  if (!calResult) {
    return Service::Error(E_NETWORK, "Failed to calibrate time to beacon: " +
                                       calResult.error().message);
  }
  timeOffsetToBeaconMs_ = calResult.value();

  // Initialize miner core
  std::filesystem::path minerDataDir =
      std::filesystem::path(getWorkDir()) / DIR_DATA;

  Miner::InitConfig minerConfig;
  minerConfig.minerId = config_.minerId;
  minerConfig.privateKeys = config_.privateKeys;
  minerConfig.timeOffsetMs = timeOffsetToBeaconMs_;
  minerConfig.workDir = minerDataDir.string();
  auto startingBlock = chooseStartingBlock(state);
  if (!startingBlock) {
    return Service::Error(E_NETWORK, startingBlock.error().message);
  }
  minerConfig.startingBlockId = startingBlock.value();

  auto minerInit = miner_.init(minerConfig);
  if (!minerInit) {
    return Service::Error(E_ROLE, "Failed to initialize Miner: " +
                                       minerInit.error().message);
  }

  orderRewindIfDue(); // a certificate kept before the node went down
  initClockFollower();
  if (upstreams_->addHints(miner_.getDialHints()) > 0) {
    upstreams_->rerank("the certificates' dial hints"); // the startup sync waits for it
  }

  // Initial catch-up: the same BlockSync as at run time; completions are
  // served here because the run loop has not started yet.
  initBlockSync();
  lastSyncResult_.reset();
  if (auto error = runStartupSync(*blockSync_, lastSyncResult_, networkTuning().startupSyncTimeout); !error.empty()) {
    return Service::Error(E_ROLE, "Failed to sync blocks from beacon: " + error);
  }

  initHandlers();

  log().info << "Miner core initialized";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake at init: " << miner_.getStake();

  log().info << "MinerServer initialization complete";
  return {};
}

MinerServer::Roe<int64_t> MinerServer::calibrateTimeToBeacon() {
  auto estimated = calibrateBlocking(
      calibrationFetch(client_, UpstreamSet::dialKey(upstreams_->activeIndex()), upstreams_->active()));
  if (!estimated) {
    return Error(E_NETWORK, estimated.error().message);
  }
  log().info << "Time calibrated through upstream [" << upstreams_->activeIndex() << "]: offset "
             << estimated.value().offsetMs << " ms (round trip " << estimated.value().rttMs << " ms, "
             << estimated.value().samples << " sample(s))";
  return estimated.value().offsetMs;
}

void MinerServer::initClockFollower() {
  clock_calibration::ClockFollower::Hooks hooks;
  hooks.generation = [this]() { return miner_.getGeneration(); };
  hooks.fetch = [this](Client::Done<Client::CalibrationResponse> done) {
    calibrationFetch(client_, UpstreamSet::dialKey(upstreams_->activeIndex()), upstreams_->active())(std::move(done));
  };
  hooks.apply = [this](int64_t offsetMs) {
    timeOffsetToBeaconMs_ = offsetMs;
    miner_.setTimeOffsetMs(offsetMs);
  };
  clock_ = std::make_unique<clock_calibration::ClockFollower>(std::move(hooks));
  clock_->redirectLogger(log().getFullName() + ".Clock");
  clock_->calibratedFor(miner_.getGeneration());
}

void MinerServer::initBlockSync() {
  BlockSync::Hooks hooks;
  hooks.dialUpstream = [this]() -> pp::Roe<void> {
    auto dial = dialUpstream(syncClient_, upstreams_->activeIndex());
    return dial ? pp::Roe<void>() : pp::Roe<void>(pp::Error(dial.error().message));
  };
  hooks.nextBlockId = [this]() { return miner_.getNextBlockId(); };
  hooks.tipHash = [this]() { return miner_.getTipHash(); };
  hooks.firstBlockHash = [this]() { return pinnedFirstBlockHash_; };
  if (!config_.network.network_anchor.predecessor_dir.empty()) {
    hooks.checkGenesis = [this, dir = config_.network.network_anchor.predecessor_dir](
                             const Ledger::ChainNode &genesis) {
      auto checked = predecessor_check::check(genesis, dir);
      if (checked) {
        log().info << "Re-genesis checked against the old ledger in " << dir;
      }
      return checked;
    };
  }
  hooks.checkStartingAuthority = [this](uint64_t blockId, const ChainAuthority &authority,
                                        const Client::Wallet &admin) {
    return checkStartingAuthority(blockId, authority, admin);
  };
  hooks.authorityTrail = [this]() -> pp::Roe<AuthorityTrail> {
    auto trail = miner_.authorityTrail();
    return trail ? pp::Roe<AuthorityTrail>(trail.value()) : pp::Roe<AuthorityTrail>(pp::Error(trail.error().message));
  };
  hooks.offerCertificate = [this](const Ledger::Record &record) { noteCertificate(record, "the upstream's STATUS"); };
  hooks.offerEquivocation = [this](const Client::Equivocation &evidence) {
    miner_.noteEquivocation(evidence, "the upstream's STATUS");
  };
  hooks.vouchedCutover = [this]() { return miner_.vouchedCutover(); };
  hooks.applyBlock = [this](const Ledger::ChainNode &fetched) -> pp::Roe<void> {
    Ledger::ChainNode block = fetched;
    block.hash = miner_.calculateHash(block.block);
    auto added = miner_.addBlock(block);
    return added ? pp::Roe<void>() : pp::Roe<void>(pp::Error(added.error().message));
  };
  hooks.postToServerThread = [this](std::function<void()> task) { postToServerThread(std::move(task)); };
  hooks.onFinished = [this](const BlockSync::Result &result) { onBlockSyncFinished(result); };
  UpstreamTipWatch::Hooks watchHooks;
  watchHooks.dialUpstream = [this]() -> pp::Roe<void> {
    auto dial = dialActiveUpstream();
    return dial ? pp::Roe<void>() : pp::Roe<void>(pp::Error(dial.error().message));
  };
  watchHooks.nextBlockId = hooks.nextBlockId;
  watchHooks.onUpstreamAhead = [this]() { blockSyncRequested_ = true; };
  watchHooks.postToServerThread = hooks.postToServerThread;
  blockSync_ = std::make_unique<BlockSync>(syncClient_, std::move(hooks));
  blockSync_->redirectLogger(log().getFullName() + ".BlockSync");
  tipWatch_ = std::make_unique<UpstreamTipWatch>(client_, std::move(watchHooks));
  tipWatch_->redirectLogger(log().getFullName() + ".TipWatch");
}

void MinerServer::warnIfNotRegisteredMiner() {
  // Once per epoch, after a successful sync (the committee is snapshotted per epoch).
  const uint64_t epoch = miner_.getCurrentEpoch();
  if (registrationCheckedEpoch_ == epoch) {
    return;
  }
  registrationCheckedEpoch_ = epoch;
  auto account = miner_.getAccount(config_.minerId);
  if (!account) {
    log().warning << "Miner account " << config_.minerId << " is not on the chain: it is never elected";
    return;
  }
  const Client::MinerState &state = account.value().wallet.miner;
  if (state.registered) {
    return;
  }
  log().warning << "Miner account " << config_.minerId << " is not a registered miner"
                << (state.autoDeregisteredEpoch != Client::MinerState::kNever
                        ? " (deregistered for missed slots in epoch " +
                              std::to_string(state.autoDeregisteredEpoch) + ")"
                        : std::string{})
                << ": it is never elected. Register with: pp-client --host <relay> -b miner-register "
                << config_.minerId << " -f <fee> -k <key>";
}

void MinerServer::onBlockSyncFinished(const BlockSync::Result &result) {
  lastSyncResult_ = result;
  orderRewindIfDue(); // the sync may have brought a certificate whose cutover is below our tip
  if (result.ok) {
    lastBlockSyncTime_ = std::chrono::steady_clock::now();
    lastSyncedEpoch_ = miner_.getCurrentEpoch();
    warnIfNotRegisteredMiner();
  } else {
    upstreams_->noteFailed(); // the next attempt (after backoff) tries another upstream
  }
  resolvePendingBlockGets();
}

void MinerServer::initHandlers() {
  // The chain reads are the base's (bindCore); what needs another server (or a sync) replies later.
  handle(Client::T_REQ_STATUS, [this](const Client::Request &request) { return hStatus(request); });
  handle(Client::T_REQ_CALIBRATION, [this](const Client::Request &) -> Roe<std::string> {
    if (!miner_.isConfigReady()) {
      return Error(E_REQUEST, "Miner syncing, please retry later");
    }
    return calibrationReply();
  });
  handleLater(Client::T_REQ_BLOCK_GET, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dBlockGet(r, reply); });
  handleLater(Client::T_REQ_TX_ADD, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dTxAdd(r, reply); });
  handleLater(Client::T_REQ_EQUIVOCATION_ADD, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dEquivocationAdd(r, reply); });
}

void MinerServer::onStop() {
  // Reply before AMP stops (Server::onStop), while replies can still be sent.
  for (auto &pending : pendingBlockGets_) {
    pending.reply(packResponse(Client::E_SERVER_ERROR, "Server stopping"));
  }
  pendingBlockGets_.clear();
  Server::onStop();
  log().info << "MinerServer resources cleaned up";
}

void MinerServer::runLoop() {
  log().info << "Block production and request handler loop started";

  while (!isStopSet()) {
    try {
      orderRewindIfDue();
      // Update miner state
      miner_.refresh();

      syncBlocksPeriodically();
      tipWatch_->maintain();
      upstreams_->maintain();
      clock_->maintain();
      if (blockSyncRequested_) {
        blockSyncRequested_ = false;
        trySyncBlocksFromBeacon(true);
      }
      renewRegistrationPeriodically();

      // Until config is loaded (late joiner waiting for T_CONFIG in synced
      // blocks) only sync; otherwise produce when we lead the slot.
      if (miner_.isConfigReady() && miner_.isSlotLeader()) {
        handleSlotLeaderRole();
      }

      // Sleep for a short time before checking again
      serveRequestsFor(std::chrono::milliseconds(100));

    } catch (const std::exception &e) {
      log().error << "Exception in block production loop: " << e.what();
      serveRequestsFor(std::chrono::seconds(1));
    }
  }

  log().info << "Block production and request handler loop stopped";
}

void MinerServer::trySyncBlocksFromBeacon(bool bypassRateLimit) {
  const uint64_t slotDurationSec = miner_.getSlotDuration();
  if (!bypassRateLimit && slotDurationSec > 0) {
    auto now = std::chrono::steady_clock::now();
    auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastBlockSyncTime_).count();
    if (elapsedSec < static_cast<int64_t>(slotDurationSec)) {
      return; // Rate limit: at most one sync per slot time
    }
  }
  if (broadcast_) {
    // Our seal is pending: applying a synced block now would be refused.
    blockSyncRequested_ = true;
    return;
  }
  if (blockSync_->start() == BlockSync::Start::BackingOff) {
    // No sync coming soon: answer waiting block gets with what we have.
    resolvePendingBlockGets();
  }
}

void MinerServer::syncBlocksPeriodically() {
  const uint64_t currentEpoch = miner_.getCurrentEpoch();
  const uint64_t currentSlot = miner_.getCurrentSlot();
  const uint64_t slotDurationSec = miner_.getSlotDuration();
  if (slotDurationSec == 0) {
    return;
  }

  // 1. At beginning of each epoch: sync to update stakeholders
  const bool needSyncForEpoch = (currentEpoch > lastSyncedEpoch_);

  // 2. Before producing: we are expected to be slot leader for next slot; sync before that slot starts
  const uint64_t nextSlot = currentSlot + 1;
  const bool weAreLeaderForNextSlot = miner_.isSlotLeaderForSlot(nextSlot);
  const int64_t nowSec = miner_.getConsensusTimestamp();
  const int64_t nextSlotStartSec = miner_.getSlotStartTime(nextSlot);
  const int64_t secUntilNextSlot = nextSlotStartSec - nowSec;
  const bool needSyncBeforeProduce =
      weAreLeaderForNextSlot && secUntilNextSlot >= 0 &&
      secUntilNextSlot <= SYNC_BEFORE_SLOT_SECONDS;

  if (!needSyncForEpoch && !needSyncBeforeProduce) {
    return;
  }

  trySyncBlocksFromBeacon(false);
}

void MinerServer::dBlockGet(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid block get payload: " + request.payload)));
    return;
  }
  const uint64_t blockId = idResult.value();
  auto result = miner_.readBlock(blockId);
  if (result) {
    reply(packResponse(result.value().ltsToString()));
    return;
  }
  if (blockId < miner_.getNextBlockId()) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    return;
  }
  // Beyond our tip: answer after the run loop's next sync instead of syncing here.
  pendingBlockGets_.push_back({blockId, reply});
  blockSyncRequested_ = true;
}

void MinerServer::resolvePendingBlockGets() {
  if (pendingBlockGets_.empty()) {
    return;
  }
  for (auto &pending : pendingBlockGets_) {
    auto result = miner_.readBlock(pending.blockId);
    if (result) {
      pending.reply(packResponse(result.value().ltsToString()));
    } else {
      replyWith(pending.reply,
                Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    }
  }
  pendingBlockGets_.clear();
}

void MinerServer::dTxAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto record = utl::binaryUnpack<Ledger::Record>(request.payload);
  if (!record) {
    replyWith(reply, Roe<std::string>(
                         Error(E_REQUEST, "Failed to deserialize transaction: " + record.error().message)));
    return;
  }
  // A beacon certificate counts here too (ba5c): a miner cut off from an honest beacon still keeps it.
  std::string kept;
  if (record.value().type == Ledger::T_BEACON_CERT) {
    if (const auto offered = noteCertificate(record.value(), "a TX_ADD");
        offered == Chain::CertificateOffer::Newer || offered == Chain::CertificateOffer::Same) {
      kept = "Beacon certificate kept";
    }
  }
  // Up the tree to the beacon's pool, where the slot leader pulls it: miners
  // never need each other's addresses.
  if (auto dial = dialActiveUpstream(); !dial) {
    replyWith(reply, kept.empty() ? Roe<std::string>(Error(E_NETWORK, dial.error().message))
                                  : Roe<std::string>(kept + "; not forwarded: " + dial.error().message));
    return;
  }
  client_.addTransactionAsync(record.value(), completeOnServerThread<void>([reply, kept](Client::Roe<void> result) {
    if (result) {
      replyWith(reply, Roe<std::string>("Transaction submitted"));
    } else {
      replyWith(reply, kept.empty() ? Roe<std::string>(Error(E_NETWORK, result.error().message))
                                    : Roe<std::string>(kept + "; the upstream refused it: " + result.error().message));
    }
  }));
}

MinerServer::Roe<std::string>
MinerServer::hStatus(const Client::Request & /*request*/) {
  Client::MinerStatus status;

  status.minerId = config_.minerId;
  status.nextBlockId = miner_.getNextBlockId();
  status.pendingTransactions = miner_.getPendingTransactionCount();
  if (!miner_.isConfigReady()) {
    status.stake = 0;
    status.currentSlot = 0;
    status.currentEpoch = 0;
    status.nStakeholders = 0;
    status.isSlotLeader = false;
  } else {
    status.stake = miner_.getStake();
    status.currentSlot = miner_.getCurrentSlot();
    status.currentEpoch = miner_.getCurrentEpoch();
    status.nStakeholders = miner_.getStakeholders().size();
    status.isSlotLeader = miner_.isSlotLeader();
  }

  return utl::binaryPack(status.ltsToMeta());
}

bool MinerServer::pullTransactionsForSlot(uint64_t slot) {
  if (txPulledSlot_ == slot) {
    return true;
  }
  const auto now = std::chrono::steady_clock::now();
  if (txPullInFlight_) {
    // Do not miss the slot waiting for the pool: produce without it after a bit.
    return now - txPullStarted_ >= txPullWait();
  }
  if (auto dial = dialActiveUpstream(); !dial) {
    log().warning << "Transaction pull: " << dial.error().message;
    txPulledSlot_ = slot;
    return true;
  }
  txPullInFlight_ = true;
  txPullStarted_ = now;
  client_.pullTransactionsAsync(
      slot, completeOnServerThread<std::vector<Ledger::Record>>(
                [this, slot](Client::Roe<std::vector<Ledger::Record>> records) {
                  txPullInFlight_ = false;
                  txPulledSlot_ = slot;
                  if (!records) {
                    log().warning << "Transaction pull failed: " << records.error().message;
                    return;
                  }
                  // A rejection is final for this slot (already included, invalid, ...);
                  // the beacon keeps what no block included for the next leader.
                  size_t added = 0;
                  for (const auto &record : records.value()) {
                    if (auto taken = miner_.addTransaction(record); taken) {
                      ++added;
                    } else {
                      log().debug << "Not taking a pulled record of type " << record.type
                                  << " for slot " << slot << ": " << taken.error().message;
                    }
                  }
                  if (added > 0) {
                    log().info << "Pulled " << added << " transaction(s) for slot " << slot;
                  }
                }));
  return false;
}

std::chrono::milliseconds MinerServer::txPullWait() const {
  const auto third = std::chrono::milliseconds(static_cast<int64_t>(miner_.getSlotDuration()) * 1000 / 3);
  return std::min(TX_PULL_MAX_WAIT, third);
}

void MinerServer::handleSlotLeaderRole() {
  if (blockSync_->inFlight() || broadcast_) {
    return; // seal only on a settled tip; try again next loop
  }
  // The leader collects for its whole slot and seals once it ends: pull the beacon's pool just
  // before the end (one pull wait), so what was submitted during the slot is in the block.
  const uint64_t slot = miner_.getProductionSlot();
  if (miner_.msUntilSlotEnd(slot) > txPullWait().count()) {
    return;
  }
  if (!pullTransactionsForSlot(slot)) {
    return;
  }

  Ledger::ChainNode block;
  auto produceResult = miner_.produceBlock(block);
  if (!produceResult) {
    log().warning << "Failed to produce block: " +
                         produceResult.error().message;
    return;
  }

  if (!produceResult.value()) {
    // No block production needed
    return;
  }

  log().info << "Successfully produced block " << block.block.index
             << " with hash " << block.hash;
  startBroadcast(block);
}

void MinerServer::startBroadcast(const Ledger::ChainNode &block) {
  const size_t upstreams = upstreams_->all().size();
  broadcast_ = PendingBroadcast{nextBroadcastId_++, block, BroadcastTally(upstreams)};
  const uint64_t id = broadcast_->id;
  for (size_t i = 0; i < upstreams; ++i) {
    // Each call captures the dial key set just before it, so the sends run in parallel.
    if (auto dial = dialUpstream(fanoutClient_, i); !dial) {
      onBroadcastResult(id, i, Client::Roe<Client::TipReply>(Client::Error(Client::E_NOT_CONNECTED, dial.error().message)));
      continue;
    }
    fanoutClient_.addBlockAsync(block, completeOnServerThread<Client::TipReply>([this, id, i](Client::Roe<Client::TipReply> result) {
      onBroadcastResult(id, i, std::move(result));
    }));
  }
  if (upstreams == 0) {
    onBroadcastResult(id, 0, Client::Roe<Client::TipReply>(Client::Error(Client::E_NOT_CONNECTED, "No upstream configured")));
  }
}

void MinerServer::onBroadcastResult(uint64_t broadcastId, size_t upstream, Client::Roe<Client::TipReply> result) {
  if (!broadcast_ || broadcast_->id != broadcastId) {
    return; // a later result for a broadcast that already finished
  }
  const auto &upstreams = upstreams_->all();
  const std::string upstreamName = upstream < upstreams.size() ? upstreams[upstream] : std::string("?");
  if (!result) {
    log().warning << "Failed to add block to upstream " << upstreamName << ": " << result.error().message;
  }
  // Committed only on the beacon's word: a signed tip, valid here, for exactly this block.
  const bool confirmed = result && signedTipCovers(result.value(), broadcast_->block);
  if (result && !confirmed) {
    log().warning << "Upstream " << upstreamName << " took block " << broadcast_->block.block.index
                  << " but sent no beacon signed tip for it";
  }
  switch (broadcast_->tally.onResult(confirmed)) {
  case BroadcastTally::Event::Commit:
    commitProducedBlock(broadcast_->block);
    break;
  case BroadcastTally::Event::AllFailed: {
    log().warning << "Failed to broadcast block " << broadcast_->block.block.index << " to any upstream";
    Ledger::ChainNode block = std::move(broadcast_->block);
    broadcast_.reset();
    // Release the uncommitted seal, or every later seal/addBlock is refused.
    miner_.abandonBlock(block);
    // Most often another leader's block won this height: take it now rather
    // than re-sealing the same stale height until the next scheduled sync —
    // and ask again who to follow: the upstream followed may be behind a
    // beacon that stood down (bp3).
    upstreams_->suspect("no upstream took block " + std::to_string(block.block.index));
    trySyncBlocksFromBeacon(true);
    return;
  }
  case BroadcastTally::Event::None:
    break;
  }
  if (broadcast_->tally.done()) {
    broadcast_.reset();
    if (blockSyncRequested_) {
      blockSyncRequested_ = false;
      trySyncBlocksFromBeacon(true);
    }
  }
}

bool MinerServer::signedTipCovers(const Client::TipReply &reply, const Ledger::ChainNode &block) const {
  const auto tip = reply.tip();
  if (!tip || tip->height != block.block.index || tip->blockHash != block.hash) {
    return false;
  }
  // Sealing applied the block, so a certificate it carries is already in force here.
  auto verified = miner_.verifySignedTip(*tip);
  if (!verified) {
    log().warning << "Signed tip for block " << tip->height << " does not verify: " << verified.error().message;
  }
  return bool(verified);
}

void MinerServer::commitProducedBlock(const Ledger::ChainNode &block) {
  log().info << "Block " << block.block.index << " broadcasted";
  miner_.markBlockProduction(block);
  auto addResult = miner_.addBlock(block);
  if (!addResult) {
    log().warning << "Failed to add block: " + addResult.error().message;
    return;
  }
  log().info << "Block produced successfully";
  log().info << "  Block ID: " << block.block.index;
  log().info << "  Slot: " << block.block.slot;
  log().info << "  Transactions: " << block.block.records.size();
  log().info << "  Hash: " << block.hash;
}

MinerServer::Roe<Client::MinerInfo> MinerServer::signedRegistration(const std::string &networkId) const {
  Client::MinerInfo minerInfo;
  minerInfo.id = config_.minerId;
  minerInfo.issuedAt = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  const std::string message = minerInfo.signingMessage(networkId);
  for (const auto &privateKey : config_.privateKeys) {
    auto signature = utl::mlDsaSign(privateKey, message);
    if (!signature) {
      return Error(E_CONFIG, "Failed to sign registration: " + signature.error().message);
    }
    minerInfo.signatures.push_back(signature.value());
  }
  return minerInfo;
}

void MinerServer::renewRegistrationPeriodically() {
  const auto now = std::chrono::steady_clock::now();
  if (registrationInFlight_ || now - lastRegistration_ < REGISTER_RENEW_INTERVAL) {
    return;
  }
  lastRegistration_ = now; // a failure retries next interval, still inside the beacon's TTL
  if (auto dial = dialActiveUpstream(); !dial) {
    log().warning << "Registration renewal: " << dial.error().message;
    return;
  }
  auto minerInfo = signedRegistration(registrationNetworkId_);
  if (!minerInfo) {
    log().warning << "Registration renewal: " << minerInfo.error().message;
    return;
  }
  registrationInFlight_ = true;
  client_.registerMinerServerAsync(
      minerInfo.value(), completeOnServerThread<Client::BeaconState>([this](Client::Roe<Client::BeaconState> state) {
        registrationInFlight_ = false;
        if (!state) {
          log().warning << "Registration renewal failed: " << state.error().message;
        }
      }));
}

MinerServer::Roe<Client::BeaconState> MinerServer::connectToBeacon() {
  if (config_.network.beacon_multiaddrs.empty()) {
    return Error(E_CONFIG, "No beacon servers configured");
  }

  // Follow the highest beacon generation, then the longest chain (ba5, bp2b). Claims are checked
  // when syncing: an upstream whose tip does not verify fails the sync and is avoided a while.
  // Rounds until one answers, within the startup sync's time: an upstream may be restarting too (a
  // rewind restarts every node at once), as a relay's startup sync retries.
  const auto deadline = std::chrono::steady_clock::now() + networkTuning().startupSyncTimeout;
  auto remaining = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  };
  while (true) {
    upstreams_->rerank("start");
    serveTasksUntil([this]() { return !upstreams_->ranking(); }, remaining());
    if (upstreams_->chosen() || isStopSet() || remaining().count() <= 0) {
      break;
    }
    log().warning << "No upstream answered yet: retrying";
    serveTasksUntil([]() { return false; }, std::min(remaining(), std::chrono::milliseconds(2000)));
  }
  if (!upstreams_->chosen()) {
    return Error(E_NETWORK, "No healthy upstream available");
  }
  log().info << "Connecting to upstream: " << upstreams_->active();

  if (auto dial = dialActiveUpstream(); !dial) {
    return Error(E_NETWORK, dial.error().message);
  }

  // Registrations are signed for the upstream's network id, so learn it first.
  auto status = client_.fetchBeaconState();
  if (!status) {
    return Error(E_NETWORK, "Failed to get upstream status: " + status.error().message);
  }
  registrationNetworkId_ = status.value().networkId;
  auto minerInfo = signedRegistration(registrationNetworkId_);
  if (!minerInfo) {
    return Error(E_CONFIG, minerInfo.error().message);
  }
  // A refused registration does not stop the start: a beacon that stood down (beacon-authority
  // part 2 — while a new one takes over) refuses them, and a miner restarting then, to rewind or by
  // its operator, must come up and wait. The run loop registers again (REGISTER_RENEW_INTERVAL).
  auto stateResult = client_.registerMinerServer(minerInfo.value());
  if (!stateResult) {
    log().warning << "Registration refused for now (retried every "
                  << std::chrono::duration_cast<std::chrono::seconds>(REGISTER_RENEW_INTERVAL).count()
                  << " s): " << stateResult.error().message;
  }
  const Client::BeaconState state = stateResult ? stateResult.value() : status.value();
  if (auto verified = verifyUpstreamState(state); !verified) {
    return Error(E_NETWORK, verified.error().message);
  }
  if (auto genesis = verifyGenesisAnchor(); !genesis) {
    return Error(E_NETWORK, genesis.error().message);
  }

  if (stateResult) {
    lastRegistration_ = std::chrono::steady_clock::now();
  }
  log().info << "Latest checkpoint ID: " << state.checkpointId;
  log().info << "Next block ID: " << state.nextBlockId;
  if (!state.networkId.empty()) {
    log().info << "Network ID: " << state.networkId;
  }

  return state;
}

void MinerServer::fillHealth(Client::NodeHealth &health) const {
  Server::fillHealth(health);
  health.role = "miner";
  if (lastSyncResult_ && lastSyncResult_->ok) {
    health.lastSyncAgeSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastBlockSyncTime_).count();
  }
  if (!isServing()) {
    return;
  }
  if (health.halted) {
    health.notReady = "halted on equivocation evidence";
  } else if (!lastSyncResult_ || !lastSyncResult_->ok) {
    health.notReady = "not synced from the upstream";
  } else {
    health.ready = true;
  }
}

} // namespace pp
