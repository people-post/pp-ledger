#include "RelayServer.h"
#include "../chain/PredecessorCheck.h"
#include "NetworkAnchor.h"
#include "BlockAddPolicy.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"
#include "../network/amp/AmpIdentity.h"
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

// ============ RunFileConfig methods ============

Object RelayServer::RunFileConfig::ltsToJson() {
  Object j;
  j.setJsonUInt("port", port);
  if (beacons.size() > 1) {
    std::vector<Value> values(beacons.begin(), beacons.end());
    j.set("beacons", Object::array(std::move(values)));
  } else {
    j.set("beacon", beacons.empty() ? std::string("<upstream multiaddr: /ip4/A.B.C.D/udp/8517/adp/1.0.0/p2p/PEER_ID>")
                                    : beacons.front());
  }
  return j;
}

namespace {
/** config.json fields a relay takes from the environment (CONFIGURATION.md § Environment). */
std::vector<env_config::Field> relayEnvFields() {
  auto fields = env_config::commonFields();
  fields.push_back({"PP_LEDGER_UPSTREAMS", "beacons", env_config::Kind::List, "", false});
  for (auto &field : env_config::anchorFields()) {
    fields.push_back(std::move(field));
  }
  return fields;
}
} // namespace

RelayServer::Roe<void>
RelayServer::RunFileConfig::ltsFromJson(const Object &jd) {
  if (jd.contains("port")) {
    auto portValue = jd.getNonNegInt("port");
    if (!portValue || *portValue == 0 || *portValue > 65535) {
      return Error(E_CONFIG, "Field 'port' must be between 1 and 65535");
    }
    port = static_cast<uint16_t>(*portValue);
  } else {
    port = DEFAULT_RELAY_PORT;
  }

  beacons.clear();
  auto addBeacon = [this](const std::string &value, const std::string &field) -> Roe<void> {
    auto ma = network::ParseBeaconMultiaddrString(value);
    if (!ma) {
      return Error(E_CONFIG, "Field '" + field + "': " + ma.error().message);
    }
    if (std::find(beacons.begin(), beacons.end(), *ma) == beacons.end()) {
      beacons.push_back(std::move(*ma));
    }
    return {};
  };
  if (jd.contains("beacon")) {
    auto beaconStr = jd.getString("beacon");
    if (!beaconStr) {
      return Error(E_CONFIG, "Field 'beacon' must be an upstream multiaddr string");
    }
    if (auto added = addBeacon(*beaconStr, "beacon"); !added) {
      return added;
    }
  }
  if (jd.contains("beacons")) {
    const Array *beaconsArr = jd.getArray("beacons");
    if (!beaconsArr) {
      return Error(E_CONFIG, "Field 'beacons' must be an array of upstream multiaddrs");
    }
    for (size_t i = 0; i < beaconsArr->elements.size(); ++i) {
      auto value = asString(beaconsArr->elements[i]);
      if (!value) {
        return Error(E_CONFIG, "beacons[" + std::to_string(i) + "] must be an upstream multiaddr string");
      }
      if (auto added = addBeacon(*value, "beacons[" + std::to_string(i) + "]"); !added) {
        return added;
      }
    }
  }
  if (beacons.empty()) {
    return Error(E_CONFIG, "Field 'beacon' (or 'beacons'), the upstream multiaddr, is required");
  }
  auto allowed = parseAllowedPeers(jd);
  if (!allowed) {
    return Error(E_CONFIG, allowed.error().message);
  }
  allowedPeers = std::move(allowed.value());
  genesisHash.reset();
  predecessorDir.clear();
  if (const Object *anchorObj = jd.getObject("networkAnchor")) {
    auto anchor = NetworkAnchor::fromJson(*anchorObj);
    if (!anchor) {
      return Error(E_CONFIG, anchor.error().message);
    }
    if (!anchor.value().genesis_hash.empty()) {
      genesisHash = anchor.value().genesis_hash;
    }
    predecessorDir = anchor.value().predecessor_dir;
  }
  return {};
}

// ============ RelayServer methods ============

RelayServer::RelayServer() {
  bindCore(relay_);
  redirectLogger("RelayServer");
  relay_.redirectLogger(log().getFullName() + ".Relay");
  client_.redirectLogger(log().getFullName() + ".Client");
  syncClient_.redirectLogger(log().getFullName() + ".SyncClient");
  fanoutClient_.redirectLogger(log().getFullName() + ".FanoutClient");
}

Client::Roe<void> RelayServer::dialUpstream(Client &client, size_t index) {
  if (!upstreams_ || index >= upstreams_->all().size()) {
    return Client::Error(Client::E_NOT_CONNECTED, "No upstream [" + std::to_string(index) + "]");
  }
  return client.setAmpPeer(UpstreamSet::dialKey(index), upstreams_->all()[index]);
}

Client::Roe<void> RelayServer::dialUpstream() {
  return dialUpstream(client_, upstreams_ ? upstreams_->activeIndex() : 0);
}

void RelayServer::initUpstreams() {
  UpstreamSet::Hooks hooks = upstreamHooks(fanoutClient_);
  hooks.admit = [this](size_t index, const Client::BeaconState &state) {
    const std::string networkId = relay_.getNetworkId();
    if (!networkId.empty() && state.networkId != networkId) {
      log().warning << "Disregarding upstream [" << index << "]: it serves network '" << state.networkId << "'";
      return false;
    }
    return true;
  };
  hooks.learn = [this](size_t index, const Client::BeaconState &state) {
    const std::string from = "upstream [" + std::to_string(index) + "]'s STATUS";
    noteCertificate(state.certificate, from);
    if (state.equivocation) {
      noteEquivocation(*state.equivocation, from, /*pushUp=*/false);
    }
  };
  hooks.offerEquivocation = [this](const Client::Equivocation &evidence, const std::string &from) {
    noteEquivocation(evidence, from, /*pushUp=*/true);
  };
  hooks.mayFollow = [this](const Client::SignedTip &tip) { return relay_.mayFollowTip(tip); };
  upstreams_ = std::make_unique<UpstreamSet>(config_.network.beacon_multiaddrs, std::move(hooks));
  upstreams_->redirectLogger(log().getFullName() + ".Upstreams");
}

void RelayServer::learnUpstreamHints() {
  if (upstreams_) {
    upstreams_->addHints(relay_.getDialHints());
  }
}

void RelayServer::rankUpstreamsAtStart() {
  learnUpstreamHints();
  if (restartRequested()) {
    return;
  }
  upstreams_->rerank("start");
  serveTasksUntil([this]() { return !upstreams_->ranking(); }, networkTuning().startupSyncTimeout);
}

void RelayServer::pushCertificateUp(const Ledger::Record &record) {
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

Service::Roe<void> RelayServer::onStart() {
  // Construct config file path
  std::filesystem::path configPath =
      std::filesystem::path(getWorkDir()) / FILE_CONFIG;
  std::string configPathStr = configPath.string();

  // Create default FILE_CONFIG if it doesn't exist using RunFileConfig
  RunFileConfig runFileConfig;

  auto loaded = env_config::loadConfig(configPathStr, relayEnvFields());
  if (!loaded) {
    return Service::Error(E_CONFIG, loaded.error().message);
  }
  if (!loaded.value()) {
    log().info << "No " << FILE_CONFIG
               << " found, creating with default values";

    auto encoded = encodeObjectPretty(runFileConfig.ltsToJson());
    if (!encoded) {
      return Service::Error(E_CONFIG, "Failed to encode " + std::string(FILE_CONFIG) +
                                          ": " + encoded.error().message);
    }
    std::ofstream configFile(configPath);
    if (!configFile) {
      return Service::Error(E_CONFIG,
                            "Failed to create " + std::string(FILE_CONFIG));
    }
    configFile << encoded.value() << std::endl;
    configFile.close();

    // A relay without an upstream cannot do anything: stop until it is set.
    return Service::Error(E_CONFIG, "Created " + configPathStr +
                                        ": set `beacon` to the upstream's multiaddr (printed by the beacon at "
                                        "start) and start again — or set PP_LEDGER_UPSTREAMS instead");
  } else if (auto parseResult = runFileConfig.ltsFromJson(*loaded.value()); !parseResult) {
    return Service::Error(E_CONFIG, "Failed to parse config (" + configPathStr + " and PP_LEDGER_* variables): " +
                                        parseResult.error().message);
  }

  // Apply configuration from RunFileConfig
  config_.network.udp_port = runFileConfig.port;
  config_.network.beacon_multiaddrs = runFileConfig.beacons;
  config_.network.genesisHash = runFileConfig.genesisHash;
  config_.network.predecessorDir = runFileConfig.predecessorDir;
  auto identityKey = loadOrCreateIdentityKey();
  if (!identityKey) {
    return Service::Error(E_CONFIG, identityKey.error().message);
  }

  log().info << "Configuration loaded";
  log().info << "  UDP port: " << config_.network.udp_port;
  for (const auto &upstream : config_.network.beacon_multiaddrs) {
    log().info << "  Upstream: " << upstream;
  }
  if (config_.network.genesisHash) {
    log().info << "  Genesis pinned (networkAnchor.genesisHash)";
  }

  if (auto upstreams = setUpstreams(config_.network.beacon_multiaddrs); !upstreams) {
    return Service::Error(E_CONFIG, upstreams.error().message);
  }
  initUpstreams();

  auto ampCfg = network::LedgerAmpConfigFromPrivateKey(identityKey.value(),
                                                       config_.network.udp_port);
  if (!ampCfg) {
    return Service::Error(E_CONFIG, "Failed to build AMP config: " + ampCfg.error().message);
  }
  setAllowedPeers(runFileConfig.allowedPeers);
  auto serverStarted = startAmpServer(*ampCfg);
  if (!serverStarted) {
    return Service::Error(E_NETWORK, "Failed to start AMP server: " +
                                         serverStarted.error().message);
  }

  if (!ampRuntime()) {
    return Service::Error(E_NETWORK, "AMP runtime unavailable after start");
  }
  client_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));
  syncClient_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));
  fanoutClient_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));

  // Initialize Relay with starting block id 0 (no beacon sync, no block
  // production)
  std::filesystem::path relayDataDir =
      std::filesystem::path(getWorkDir()) / DIR_DATA;
  Relay::InitConfig relayConfig;
  relayConfig.workDir = relayDataDir.string();
  relayConfig.timeOffsetMs = 0; // calibrated once the upstream to follow is chosen (below)
  relayConfig.startingBlockId = 0;

  auto relayInit = relay_.init(relayConfig);
  if (!relayInit) {
    return Service::Error(E_ROLE, "Failed to initialize Relay: " +
                                       relayInit.error().message);
  }

  orderRewindIfDue(); // a certificate kept before the node went down
  // A relay that already holds the genesis checks it again: predecessorDir may be new (B037).
  if (!config_.network.predecessorDir.empty() && relay_.getNextBlockId() > 0) {
    auto genesis = relay_.readBlock(0);
    if (!genesis) {
      return Service::Error(E_CONFIG, "Cannot read the genesis to check its predecessor: " + genesis.error().message);
    }
    if (auto checked = predecessor_check::check(genesis.value(), config_.network.predecessorDir); !checked) {
      return Service::Error(E_CONFIG, "Genesis refused: " + checked.error().message);
    }
    log().info << "Re-genesis checked against the old ledger in " << config_.network.predecessorDir;
  }
  rankUpstreamsAtStart();
  if (auto offset = calibrateTimeToBeacon(); offset) {
    applyTimeOffset(offset.value());
  } else {
    log().warning << "Time calibration skipped: " << offset.error().message;
  }
  initClockFollower();

  // Initial catch-up through the same BlockSync as at run time; completions
  // are served here because the run loop has not started yet.
  initBlockSync();
  lastSyncResult_.reset();
  if (auto error = runStartupSync(*blockSync_, lastSyncResult_, networkTuning().startupSyncTimeout,
                                  [this]() { return !beaconStateRefreshInFlight_; });
      !error.empty()) {
    return Service::Error(E_NETWORK, "Failed to sync blocks from beacon: " + error);
  }

  log().info << "Relay core initialized";
  log().info << "  Next block ID: " << relay_.getNextBlockId();

  initHandlers();
  log().info << "RelayServer initialization complete";
  return {};
}

void RelayServer::initBlockSync() {
  BlockSync::Hooks hooks;
  hooks.dialUpstream = [this]() -> pp::Roe<void> {
    auto dial = dialUpstream(syncClient_, upstreams_->activeIndex());
    return dial ? pp::Roe<void>() : pp::Roe<void>(pp::Error(dial.error().message));
  };
  hooks.nextBlockId = [this]() { return relay_.getNextBlockId(); };
  hooks.tipHash = [this]() { return relay_.getTipHash(); };
  hooks.firstBlockHash = [this]() { return config_.network.genesisHash; }; // a relay starts at genesis
  if (!config_.network.predecessorDir.empty()) {
    hooks.checkGenesis = [this, dir = config_.network.predecessorDir](const Ledger::ChainNode &genesis) {
      auto checked = predecessor_check::check(genesis, dir);
      if (checked) {
        log().info << "Re-genesis checked against the old ledger in " << dir;
      }
      return checked;
    };
  }
  hooks.authorityTrail = [this]() -> pp::Roe<AuthorityTrail> {
    auto trail = relay_.authorityTrail();
    return trail ? pp::Roe<AuthorityTrail>(trail.value()) : pp::Roe<AuthorityTrail>(pp::Error(trail.error().message));
  };
  hooks.onTipVerified = [this](const Client::SignedTip &tip) { noteSignedTip(tip); };
  hooks.offerCertificate = [this](const Ledger::Record &record) { noteCertificate(record, "the upstream's STATUS"); };
  hooks.offerEquivocation = [this](const Client::Equivocation &evidence) {
    noteEquivocation(evidence, "the upstream's STATUS", /*pushUp=*/false);
  };
  hooks.vouchedCutover = [this]() { return relay_.vouchedCutover(); };
  hooks.applyBlock = [this](const Ledger::ChainNode &fetched) -> pp::Roe<void> {
    Ledger::ChainNode block = fetched;
    block.hash = relay_.calculateHash(block.block);
    auto added = relay_.addBlock(block);
    return added ? pp::Roe<void>() : pp::Roe<void>(pp::Error(added.error().message));
  };
  hooks.postToServerThread = [this](std::function<void()> task) { postToServerThread(std::move(task)); };
  hooks.onFinished = [this](const BlockSync::Result &result) { onBlockSyncFinished(result); };
  UpstreamTipWatch::Hooks watchHooks;
  watchHooks.dialUpstream = [this]() -> pp::Roe<void> {
    auto dial = dialUpstream();
    return dial ? pp::Roe<void>() : pp::Roe<void>(pp::Error(dial.error().message));
  };
  watchHooks.nextBlockId = hooks.nextBlockId;
  watchHooks.onUpstreamAhead = [this]() { blockSyncRequested_ = true; };
  watchHooks.onSignedTip = [this](const Client::SignedTip &tip) { noteSignedTip(tip); };
  watchHooks.postToServerThread = hooks.postToServerThread;
  blockSync_ = std::make_unique<BlockSync>(syncClient_, std::move(hooks));
  blockSync_->redirectLogger(log().getFullName() + ".BlockSync");
  tipWatch_ = std::make_unique<UpstreamTipWatch>(client_, std::move(watchHooks));
  tipWatch_->redirectLogger(log().getFullName() + ".TipWatch");
}

void RelayServer::onBlockSyncFinished(const BlockSync::Result &result) {
  lastSyncResult_ = result;
  orderRewindIfDue(); // the sync may have brought a certificate whose cutover is below our tip
  if (result.ok) {
    lastBlockSyncTime_ = std::chrono::steady_clock::now();
    lastSyncedEpoch_ = relay_.getCurrentEpoch();
    startBeaconStateRefresh();
  } else {
    upstreams_->noteFailed(); // the next attempt (after backoff) tries another upstream
  }
  resolvePendingBlockGets();
}

void RelayServer::startBeaconStateRefresh() {
  if (beaconStateRefreshInFlight_) {
    return;
  }
  beaconStateRefreshInFlight_ = true;
  client_.fetchBeaconStateAsync(completeOnServerThread<Client::BeaconState>(
      [this](Client::Roe<Client::BeaconState> status) {
        beaconStateRefreshInFlight_ = false;
        if (!status) {
          log().warning << "Failed to refresh beacon state: " << status.error().message;
          return;
        }
        registryVersion_ = status.value().registryVersion;
        if (!status.value().networkId.empty()) {
          networkId_ = status.value().networkId;
        }
        noteCertificate(status.value().certificate, "the upstream's STATUS");
        if (const auto &evidence = status.value().equivocation) {
          noteEquivocation(*evidence, "the upstream's STATUS", /*pushUp=*/false);
        }
        noteSignedTip(status.value().signedTip);
      }));
}

bool RelayServer::coversBlock(const Client::TipReply &reply, const Ledger::ChainNode &block) const {
  const auto tip = reply.tip();
  // The block may itself certify the generation that signed the tip.
  return tip && tip->height == block.block.index && tip->blockHash == relay_.calculateHash(block.block) &&
         relay_.verifySignedTip(*tip, {&block.block});
}

void RelayServer::noteSignedTip(const std::optional<Client::SignedTip> &tip) {
  if (!tip) {
    return;
  }
  if (auto verified = relay_.verifySignedTip(*tip); !verified) {
    log().debug << "Ignoring signed tip for block " << tip->height << ": " << verified.error().message;
    return;
  }
  if (upstreamTip_.offer(*tip) == UpstreamSignedTip::Offer::Conflict) {
    // Both verified: the beacon key signed two blocks at one height (ba6).
    if (const auto evidence = Client::Equivocation::of(*upstreamTip_.latest(), *tip)) {
      noteEquivocation(*evidence, "the upstream's signed tips", /*pushUp=*/true);
    }
  }
}

std::optional<Chain::EquivocationOffer> RelayServer::noteEquivocation(const Client::Equivocation &evidence,
                                                                      const std::string &from, bool pushUp) {
  auto offered = relay_.noteEquivocation(evidence, from);
  if (offered == Chain::EquivocationOffer::Newer) {
    dropHeldTipIfInvalid();
    if (pushUp) {
      pushEquivocationUp(evidence);
    }
  }
  return offered;
}

void RelayServer::pushEquivocationUp(const Client::Equivocation &evidence) {
  // So the beacon halts too — each beacon host, if a split runs two; downstreams learn it from this
  // relay's STATUS.
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

void RelayServer::dEquivocationAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  Client::Equivocation evidence;
  if (!evidence.ltsFromString(request.payload)) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid equivocation evidence")));
    return;
  }
  const auto offered = noteEquivocation(evidence, "an EQUIVOCATION_ADD", /*pushUp=*/false);
  const std::string kept = offered == Chain::EquivocationOffer::Newer || offered == Chain::EquivocationOffer::Same
                               ? "Equivocation evidence kept"
                               : "Equivocation evidence not kept here";
  // Up the tree whatever this relay made of it: the beacon judges for itself.
  if (auto dial = dialUpstream(); !dial) {
    replyWith(reply, Roe<std::string>(kept + "; not forwarded: " + dial.error().message));
    return;
  }
  client_.addEquivocationAsync(evidence, completeOnServerThread<std::string>([reply, kept](Client::Roe<std::string> result) {
    replyWith(reply, Roe<std::string>(kept + "; upstream: " + (result ? result.value() : result.error().message)));
  }));
}

std::optional<Chain::CertificateOffer> RelayServer::noteCertificate(const std::optional<Ledger::Record> &record,
                                                                    const std::string &from) {
  if (!record) {
    return std::nullopt;
  }
  auto offered = relay_.noteCertificate(*record, from);
  if (offered == Chain::CertificateOffer::Newer) {
    dropHeldTipIfInvalid();
    if (upstreams_) {
      // bp2b: up the tree, to the new beacon's gateways, and follow whichever serves the new generation.
      pushCertificateUp(*record);
      learnUpstreamHints();
      upstreams_->rerank("a new beacon certificate");
    }
  }
  return offered;
}

void RelayServer::dropHeldTipIfInvalid() {
  const auto &tip = upstreamTip_.latest();
  if (!tip) {
    return;
  }
  if (auto verified = relay_.verifySignedTip(*tip); !verified) {
    log().warning << "No longer serving the signed tip for block " << tip->height << ": "
                  << verified.error().message;
    upstreamTip_.reset();
  }
}

void RelayServer::initHandlers() {
  // The chain reads and CALIBRATION are the base's (bindCore); what needs the upstream (or a sync)
  // replies later.
  handle(Client::T_REQ_STATUS, [this](const Client::Request &request) { return hStatus(request); });
  handleLater(Client::T_REQ_BLOCK_GET, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dBlockGet(r, reply); });
  handleLater(Client::T_REQ_BLOCK_ADD, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dBlockAdd(r, reply); });
  handleLater(Client::T_REQ_REGISTER, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dRegister(r, reply); });
  handleLater(Client::T_REQ_MINER_LIST, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dMinerList(r, reply); });
  handleLater(Client::T_REQ_TX_ADD, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dTxAdd(r, reply); });
  handleLater(Client::T_REQ_EQUIVOCATION_ADD, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dEquivocationAdd(r, reply); });
  handleLater(Client::T_REQ_TX_PULL, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dTxPull(r, reply); });
}

void RelayServer::onStop() {
  // Reply before AMP stops (Server::onStop), while replies can still be sent.
  for (auto &pending : pendingBlockGets_) {
    pending.reply(packResponse(Client::E_SERVER_ERROR, "Server stopping"));
  }
  pendingBlockGets_.clear();
  Server::onStop();
  log().info << "RelayServer resources cleaned up";
}

Client::BeaconState RelayServer::buildStateResponse() const {
  int64_t currentTimestamp =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  Client::BeaconState state;
  state.currentTimestamp = currentTimestamp;
  const auto checkpoint = relay_.getCheckpoint();
  state.checkpointId = checkpoint.lastId;  // User lastId so that miners can replay blocks to current checkpoint
  state.nextBlockId = relay_.getNextBlockId();
  state.currentSlot = relay_.getCurrentSlot();
  state.currentEpoch = relay_.getCurrentEpoch();
  state.nStakeholders = relay_.getStakeholders().size();
  state.networkId = networkId_;
  state.registryVersion = registryVersion_;
  state.signedTip = upstreamTip_.latest();
  state.equivocation = relay_.getEquivocation();
  state.recovery = relay_.getRecoveryStatus();
  state.rewound = relay_.getLastRewind();
  state.certificate = relay_.getCurrentCertificate();
  if (state.nextBlockId > 0) {
    if (auto tip = relay_.readBlock(state.nextBlockId - 1)) {
      state.headHash = tip.value().hash;
    }
  }

  return state;
}

void RelayServer::runLoop() {
  log().info << "Request handler loop started";

  while (!isStopSet()) {
    try {
      orderRewindIfDue();
      relay_.refresh();
      syncBlocksPeriodically();
      tipWatch_->maintain();
      upstreams_->maintain();
      clock_->maintain();
      if (blockSyncRequested_) {
        blockSyncRequested_ = false;
        trySyncBlocksFromBeacon(true);
      }
      serveRequestsFor(std::chrono::milliseconds(100));
    } catch (const std::exception& e) {
      log().error << "Exception in request handler loop: " << e.what();
      serveRequestsFor(std::chrono::seconds(1));
    }
  }

  log().info << "Request handler loop stopped";
}

void RelayServer::trySyncBlocksFromBeacon(bool bypassRateLimit) {
  const uint64_t slotDurationSec = relay_.getSlotDuration();
  if (!bypassRateLimit && slotDurationSec > 0) {
    auto now = std::chrono::steady_clock::now();
    auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastBlockSyncTime_).count();
    if (elapsedSec < static_cast<int64_t>(slotDurationSec)) {
      return; // Rate limit: at most one sync per slot time
    }
  }
  if (blockSync_->start() == BlockSync::Start::BackingOff) {
    // No sync coming soon: answer waiting block gets with what we have.
    resolvePendingBlockGets();
  }
}

void RelayServer::syncBlocksPeriodically() {
  const uint64_t currentEpoch = relay_.getCurrentEpoch();
  const uint64_t slotDurationSec = relay_.getSlotDuration();
  if (slotDurationSec == 0) {
    return;
  }

  // At beginning of each epoch: sync to update stakeholders (relay never produces blocks)
  const bool needSyncForEpoch = (currentEpoch > lastSyncedEpoch_);
  if (!needSyncForEpoch) {
    return;
  }

  trySyncBlocksFromBeacon(false);
}

void RelayServer::dBlockGet(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid block get payload: " + request.payload)));
    return;
  }
  const uint64_t blockId = idResult.value();
  auto result = relay_.readBlock(blockId);
  if (result) {
    reply(packResponse(result.value().ltsToString()));
    return;
  }
  if (blockId < relay_.getNextBlockId()) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    return;
  }
  // Beyond our tip: answer after the run loop's next sync instead of syncing here.
  pendingBlockGets_.push_back({blockId, reply});
  blockSyncRequested_ = true;
}

void RelayServer::resolvePendingBlockGets() {
  if (pendingBlockGets_.empty()) {
    return;
  }
  for (auto &pending : pendingBlockGets_) {
    auto result = relay_.readBlock(pending.blockId);
    if (result) {
      pending.reply(packResponse(result.value().ltsToString()));
    } else {
      replyWith(pending.reply,
                Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    }
  }
  pendingBlockGets_.clear();
}

void RelayServer::dBlockAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  Ledger::ChainNode block;
  if (!block.ltsFromString(request.payload)) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to deserialize block: " + request.payload)));
    return;
  }
  switch (checkBlockAdd(relay_, block)) {
  case BlockAddCheck::AlreadyHave: { // we synced it from upstream: it is there already
    Client::TipReply known;
    known.nextBlockId = relay_.getNextBlockId();
    known.setTip(upstreamTip_.latest());
    reply(packResponse(utl::binaryPack(known)));
    return;
  }
  case BlockAddCheck::Conflicts:
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Block " + std::to_string(block.block.index) +
                                                           " conflicts with the stored block")));
    return;
  case BlockAddCheck::New:
    break;
  }
  if (auto dial = dialUpstream(); !dial) {
    replyWith(reply, Roe<std::string>(Error(E_NETWORK, "Failed to dial beacon: " + dial.error().message)));
    return;
  }
  client_.addBlockAsync(block, completeOnServerThread<Client::TipReply>([this, block, reply](
                                                                            Client::Roe<Client::TipReply> result) {
    if (!result) {
      // The upstream followed refused a block: it may stand behind a beacon that stood down (bp3).
      upstreams_->suspect("the upstream refused block " + std::to_string(block.block.index));
      replyWith(reply, Roe<std::string>(Error(E_NETWORK, result.error().message)));
      return;
    }
    // Store it now only if the beacon's signed tip vouches for it; otherwise sync brings it.
    if (coversBlock(result.value(), block)) {
      relay_.addBlock(block); // Don't care about the result
    }
    noteSignedTip(result.value().tip()); // after the block: it may certify the tip's generation
    reply(packResponse(utl::binaryPack(result.value()))); // the beacon's reply, unchanged
  }));
}

void RelayServer::dRegister(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto unpacked = utl::binaryUnpack<pp::common::Meta>(request.payload);
  if (!unpacked) {
    replyWith(reply, Roe<std::string>(
                         Error(E_REQUEST, "Failed to unpack miner info Meta: " + unpacked.error().message)));
    return;
  }
  Client::MinerInfo minerInfo;
  auto parsed = minerInfo.ltsFromMeta(unpacked.value());
  if (!parsed) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, parsed.error().message)));
    return;
  }
  if (!dialUpstreamFor(reply)) {
    return;
  }
  client_.registerMinerServerAsync(
      minerInfo, completeOnServerThread<Client::BeaconState>(
                     [this, reply](Client::Roe<Client::BeaconState> state) {
                       if (!state) {
                         replyWith(reply, Roe<std::string>(Error(
                                              E_NETWORK, "Upstream register failed: " + state.error().message)));
                         return;
                       }
                       registryVersion_ = state.value().registryVersion;
                       if (!state.value().networkId.empty()) {
                         networkId_ = state.value().networkId;
                       }
                       noteSignedTip(state.value().signedTip);
                       reply(packResponse(utl::binaryPack(state.value().ltsToMeta())));
                     }));
}

RelayServer::Roe<std::string>
RelayServer::hStatus(const Client::Request & /*request*/) {
  return utl::binaryPack(buildStateResponse().ltsToMeta());
}

RelayServer::Roe<int64_t> RelayServer::calibrateTimeToBeacon() {
  // The upstream followed first, then any other that answers.
  Roe<int64_t> offset = Error(E_NETWORK, "No upstream configured");
  const size_t count = upstreams_->all().size();
  for (size_t step = 0; step < count; ++step) {
    const size_t i = (upstreams_->activeIndex() + step) % count;
    auto estimated =
        calibrateBlocking(calibrationFetch(client_, UpstreamSet::dialKey(i), upstreams_->all()[i]));
    if (estimated) {
      log().info << "Time calibrated through upstream [" << i << "]: offset " << estimated.value().offsetMs
                 << " ms (round trip " << estimated.value().rttMs << " ms, " << estimated.value().samples
                 << " sample(s))";
      return estimated.value().offsetMs;
    }
    log().debug << "Time calibration through upstream [" << i << "] failed: " << estimated.error().message;
    offset = Error(E_NETWORK, estimated.error().message);
  }
  return offset;
}

void RelayServer::initClockFollower() {
  clock_calibration::ClockFollower::Hooks hooks;
  hooks.generation = [this]() { return relay_.getGeneration(); };
  hooks.fetch = [this](Client::Done<Client::CalibrationResponse> done) {
    calibrationFetch(client_, UpstreamSet::dialKey(upstreams_->activeIndex()), upstreams_->active())(std::move(done));
  };
  hooks.apply = [this](int64_t offsetMs) { applyTimeOffset(offsetMs); };
  clock_ = std::make_unique<clock_calibration::ClockFollower>(std::move(hooks));
  clock_->redirectLogger(log().getFullName() + ".Clock");
  clock_->calibratedFor(relay_.getGeneration());
}

void RelayServer::applyTimeOffset(int64_t offsetMs) {
  timeOffsetToBeaconMs_ = offsetMs;
  relay_.setTimeOffsetMs(offsetMs);
}

bool RelayServer::dialUpstreamFor(const RequestQueue::Reply &reply) {
  if (auto dial = dialUpstream(); !dial) {
    replyWith(reply, Roe<std::string>(Error(E_NETWORK, "Failed to dial upstream: " + dial.error().message)));
    return false;
  }
  return true;
}

void RelayServer::dTxAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto record = utl::binaryUnpack<Ledger::Record>(request.payload);
  if (!record) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to deserialize transaction: " + record.error().message)));
    return;
  }
  // A beacon certificate counts here too (ba5c): a relay cut off from an honest beacon still keeps it.
  std::string kept;
  if (record.value().type == Ledger::T_BEACON_CERT) {
    if (const auto offered = noteCertificate(record.value(), "a TX_ADD");
        offered == Chain::CertificateOffer::Newer || offered == Chain::CertificateOffer::Same) {
      kept = "Beacon certificate kept";
    }
  }
  if (auto dial = dialUpstream(); !dial) {
    replyWith(reply, kept.empty() ? Roe<std::string>(Error(E_NETWORK, "Failed to dial upstream: " + dial.error().message))
                                  : Roe<std::string>(kept + "; not forwarded: " + dial.error().message));
    return;
  }
  // Up the tree to the beacon's pool; the slot leader pulls it from there.
  client_.addTransactionAsync(record.value(), completeOnServerThread<void>([reply, kept](Client::Roe<void> result) {
    if (result) {
      replyWith(reply, Roe<std::string>("Transaction submitted"));
    } else {
      replyWith(reply, kept.empty() ? Roe<std::string>(Error(E_NETWORK, result.error().message))
                                    : Roe<std::string>(kept + "; the upstream refused it: " + result.error().message));
    }
  }));
}

void RelayServer::dTxPull(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto slot = utl::binaryUnpack<uint64_t>(request.payload);
  if (!slot) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid transaction pull payload")));
    return;
  }
  if (!dialUpstreamFor(reply)) {
    return;
  }
  client_.pullTransactionsAsync(
      slot.value(), completeOnServerThread<std::vector<Ledger::Record>>(
                        [reply](Client::Roe<std::vector<Ledger::Record>> records) {
                          replyWith(reply, records ? Roe<std::string>(utl::binaryPack(records.value()))
                                                   : Roe<std::string>(Error(E_NETWORK, records.error().message)));
                        }));
}

void RelayServer::dMinerList(const Client::Request & /*request*/, const RequestQueue::Reply &reply) {
  if (!dialUpstreamFor(reply)) {
    return;
  }
  client_.fetchMinerListAsync(completeOnServerThread<std::vector<Client::MinerInfo>>(
      [reply](Client::Roe<std::vector<Client::MinerInfo>> miners) {
        if (!miners) {
          replyWith(reply, Roe<std::string>(Error(
                               E_NETWORK, "Failed to fetch miner list from upstream: " + miners.error().message)));
          return;
        }
        std::vector<pp::common::Meta> list;
        list.reserve(miners.value().size());
        for (const auto &miner : miners.value()) {
          list.push_back(miner.ltsToMeta());
        }
        reply(packResponse(utl::binaryPack(list)));
      }));
}

namespace {
/** A relay or miner this many blocks behind its upstream's signed tip is not ready. */
constexpr uint64_t kReadyLagBlocks = 3;
} // namespace

void RelayServer::fillHealth(Client::NodeHealth &health) const {
  Server::fillHealth(health);
  health.role = "relay";
  health.networkId = networkId_;
  if (health.signedTipHeight) {
    health.upstreamNextBlockId = *health.signedTipHeight + 1;
  }
  if (lastSyncResult_ && lastSyncResult_->ok) {
    health.lastSyncAgeSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastBlockSyncTime_).count();
  }
  if (!isServing()) {
    return;
  }
  const uint64_t lag = health.upstreamNextBlockId && *health.upstreamNextBlockId > health.nextBlockId
                           ? *health.upstreamNextBlockId - health.nextBlockId
                           : 0;
  if (health.halted) {
    health.notReady = "halted on equivocation evidence";
  } else if (!lastSyncResult_ || !lastSyncResult_->ok) {
    health.notReady = "not synced from the upstream";
  } else if (lag > kReadyLagBlocks) {
    health.notReady = std::to_string(lag) + " blocks behind the upstream";
  } else {
    health.ready = true;
  }
}

} // namespace pp
