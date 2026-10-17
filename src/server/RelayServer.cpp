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

RelayServer::RelayServer() : FollowerServer("RelayServer") {
  relay_.redirectLogger(log().getFullName() + ".Relay");
  bindFollower(relay_);
  initHandlers();
}

Service::Roe<void> RelayServer::admitUpstream(const Client::BeaconState &state) const {
  const std::string networkId = relay_.getNetworkId();
  if (!networkId.empty() && state.networkId != networkId) {
    return Error(E_CONFIG, "it serves network '" + state.networkId + "'");
  }
  return {};
}

void RelayServer::fillHealth(Client::NodeHealth &health) const {
  FollowerServer::fillHealth(health);
  health.role = "relay";
  health.networkId = networkId_;
}

void RelayServer::rankUpstreamsAtStart() {
  learnUpstreamHints();
  if (restartRequested()) {
    return;
  }
  upstreams_->rerank("start");
  serveTasksUntil([this]() { return !upstreams_->ranking(); }, networkTuning().startupSyncTimeout);
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

  if (auto started = startFollowing(config_.network.beacon_multiaddrs, identityKey.value(), config_.network.udp_port,
                                    runFileConfig.allowedPeers);
      !started) {
    return started;
  }

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
  // Initial catch-up through the same BlockSync as at run time; completions are served here because
  // the run loop has not started yet.
  if (auto error = catchUp([this]() { return !beaconStateRefreshInFlight_; }); !error.empty()) {
    return Service::Error(E_NETWORK, "Failed to sync blocks from beacon: " + error);
  }

  log().info << "Relay core initialized";
  log().info << "  Next block ID: " << relay_.getNextBlockId();

  log().info << "RelayServer initialization complete";
  return {};
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
        if (const auto &certificate = status.value().certificate) {
          noteCertificate(*certificate, "the upstream's STATUS");
        }
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
  // The chain reads and CALIBRATION are the base's, BLOCK_GET, TX_ADD and EQUIVOCATION_ADD the
  // follower's (bindFollower); what the upstream answers replies later.
  handle(Client::T_REQ_STATUS, [this](const Client::Request &request) { return hStatus(request); });
  handleLater(Client::T_REQ_BLOCK_ADD, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dBlockAdd(r, reply); });
  handleLater(Client::T_REQ_REGISTER, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dRegister(r, reply); });
  handleLater(Client::T_REQ_MINER_LIST, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dMinerList(r, reply); });
  handleLater(Client::T_REQ_TX_PULL, [this](const Client::Request &r, const RequestQueue::Reply &reply) { dTxPull(r, reply); });
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

} // namespace pp
