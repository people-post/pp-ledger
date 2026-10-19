#include "ledger/server/RelayServer.h"
#include "ledger/chain/PredecessorCheck.h"
#include "ledger/server/NetworkAnchor.h"
#include "ledger/server/BlockAddPolicy.h"
#include "ledger/client/AccountIds.h"
#include "ledger/client/Client.h"
#include "ledger/ledger/Ledger.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "ledger/lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

namespace pp {

using pp::common::Object;

// ============ RelayServer methods ============

RelayServer::RelayServer() : FollowerServer("RelayServer") {
  relay_.redirectLogger(log().getFullName() + ".Relay");
  bindFollower(relay_);
  initHandlers();
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
  FollowerConfig config(Client::DEFAULT_RELAY_PORT);
  Object configTemplate;
  config.toJson(configTemplate, "<upstream multiaddr: /ip4/A.B.C.D/udp/8517/adp/1.0.0/p2p/PEER_ID>");
  auto fields = env_config::commonFields();
  for (auto &field : FollowerConfig::envFields()) {
    fields.push_back(std::move(field));
  }
  // A relay without an upstream cannot do anything: stop until it is set.
  auto loaded = loadRunConfig(fields, configTemplate,
                              "set `beacons` to the upstream's multiaddr (printed by the beacon at start) and start "
                              "again — or set PP_LEDGER_UPSTREAMS instead");
  if (!loaded) {
    return loaded.error();
  }
  if (auto parsed = config.fromJson(loaded.value()); !parsed) {
    return Service::Error(E_CONFIG, "Failed to parse config (config.json and PP_LEDGER_* variables): " +
                                        parsed.error().message);
  }
  log().info << "Configuration loaded";
  log().info << "  UDP port: " << config.port;
  for (const auto &upstream : config.upstreams) {
    log().info << "  Upstream: " << upstream;
  }
  if (!config.anchor.genesis_hash.empty()) {
    log().info << "  Genesis pinned (networkAnchor.genesisHash)";
  }

  if (auto started = startFollowing(config, [this]() { return loadOrCreateIdentityKey(); }); !started) {
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
  if (const std::string &dir = followerConfig().anchor.predecessor_dir; !dir.empty() && relay_.getNextBlockId() > 0) {
    auto genesis = relay_.readBlock(0);
    if (!genesis) {
      return Service::Error(E_CONFIG, "Cannot read the genesis to check its predecessor: " + genesis.error().message);
    }
    if (auto checked = predecessor_check::check(genesis.value(), dir); !checked) {
      return Service::Error(E_CONFIG, "Genesis refused: " + checked.error().message);
    }
    log().info << "Re-genesis checked against the old ledger in " << dir;
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
  const auto budget = forwardOrExpire(request, reply);
  if (!budget) {
    return;
  }
  client_.addBlockAsync(block, completeOnServerThread<Client::TipReply>([this, block, reply](
                                                                            Client::Roe<Client::TipReply> result) {
    if (!result) {
      // The upstream followed refused a block: it may stand behind a beacon that stood down (bp3).
      upstreams_->suspect("the upstream refused block " + std::to_string(block.block.index));
      replyUpstreamError(reply, result.error());
      return;
    }
    // Store it now only if the beacon's signed tip vouches for it; otherwise sync brings it.
    if (coversBlock(result.value(), block)) {
      relay_.addBlock(block); // Don't care about the result
    }
    noteSignedTip(result.value().tip()); // after the block: it may certify the tip's generation
    reply(packResponse(utl::binaryPack(result.value()))); // the beacon's reply, unchanged
  }), *budget);
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
  // Checked here before it costs the beacon anything, as transfers are (lr8): a flood of forged
  // registrations stops at the relay. An account the relay does not hold yet (it may be a block behind)
  // is the beacon's to judge.
  if (AccountIds::isSystemAccount(minerInfo.id)) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "System account " + std::to_string(minerInfo.id) +
                                                           " cannot register as a miner")));
    return;
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
  if (std::llabs(now - minerInfo.issuedAt) >
      std::chrono::duration_cast<std::chrono::seconds>(Client::MinerInfo::kMaxIssuedSkew).count()) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Registration timestamp is too far from the relay's clock")));
    return;
  }
  if (auto signed_ = relay_.verifyAccountSignatures(minerInfo.id, minerInfo.signingMessage(relay_.getNetworkId()),
                                                    minerInfo.signatures);
      !signed_ && signed_.error().code != Chain::E_ACCOUNT_NOT_FOUND) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Registration not signed by miner " +
                                                           std::to_string(minerInfo.id) + ": " +
                                                           signed_.error().message)));
    return;
  }
  if (!dialUpstreamFor(reply)) {
    return;
  }
  const auto budget = forwardOrExpire(request, reply);
  if (!budget) {
    return;
  }
  client_.registerMinerServerAsync(
      minerInfo, completeOnServerThread<Client::BeaconState>(
                     [this, reply, from = request.from](Client::Roe<Client::BeaconState> state) {
                       if (!state) {
                         replyUpstreamError(reply, state.error(), "Upstream register failed: ");
                         return;
                       }
                       noteMinerPeer(from); // the beacon took it: its sync and blocks go first (LR009)
                       registryVersion_ = state.value().registryVersion;
                       if (!state.value().networkId.empty()) {
                         networkId_ = state.value().networkId;
                       }
                       noteSignedTip(state.value().signedTip);
                       reply(packResponse(utl::binaryPack(state.value().ltsToMeta())));
                     }),
      *budget);
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
  const auto budget = forwardOrExpire(request, reply);
  if (!budget) {
    return;
  }
  client_.pullTransactionsAsync(
      slot.value(),
      completeOnServerThread<std::vector<Ledger::Record>>([reply](Client::Roe<std::vector<Ledger::Record>> records) {
        if (records) {
          reply(packResponse(utl::binaryPack(records.value())));
        } else {
          replyUpstreamError(reply, records.error());
        }
      }),
      *budget);
}

void RelayServer::dMinerList(const Client::Request &request, const RequestQueue::Reply &reply) {
  if (!dialUpstreamFor(reply)) {
    return;
  }
  const auto budget = forwardOrExpire(request, reply);
  if (!budget) {
    return;
  }
  client_.fetchMinerListAsync(completeOnServerThread<std::vector<Client::MinerInfo>>(
      [reply](Client::Roe<std::vector<Client::MinerInfo>> miners) {
        if (!miners) {
          replyUpstreamError(reply, miners.error(), "Failed to fetch miner list from upstream: ");
          return;
        }
        std::vector<pp::common::Meta> list;
        list.reserve(miners.value().size());
        for (const auto &miner : miners.value()) {
          list.push_back(miner.ltsToMeta());
        }
        reply(packResponse(utl::binaryPack(list)));
      }), *budget);
}

} // namespace pp
