#include "server/MinerServer.h"
#include "chain/PredecessorCheck.h"
#include "chain/BlockValidation.h"
#include "ledger/client/Client.h"
#include "ledger/ledger/Ledger.h"
#include "ledger/network/amp/AmpIdentity.h"
#include "server/NetworkAnchor.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "common/Logger.h"
#include "ledger/lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

namespace pp {

using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using pp::common::asString;

MinerServer::Roe<void> MinerServer::RunFileConfig::fromJson(const Object &jd) {
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
  if (auto follower = FollowerConfig::fromJson(jd); !follower) {
    return Error(E_CONFIG, follower.error().message);
  }
  return {};
}

Object MinerServer::RunFileConfig::toJson() const {
  Object j;
  j.setUIntForJson("minerId", minerId);
  std::vector<Value> keyValues(keys.begin(), keys.end());
  if (keyValues.empty()) {
    keyValues.push_back(std::string("<private key file: pp-client keygen -o key>"));
  }
  j.set("keys", Object::array(std::move(keyValues)));
  FollowerConfig::toJson(j, "<upstream multiaddr: /ip4/A.B.C.D/udp/8519/adp/1.0.0/p2p/PEER_ID>");
  return j;
}

// ============ MinerServer methods ============

MinerServer::MinerServer() : FollowerServer("MinerServer") {
  miner_.redirectLogger(log().getFullName() + ".Miner");
  bindFollower(miner_);
  initHandlers();
}

std::optional<std::string> MinerServer::firstBlockHash() const { return pinnedFirstBlockHash_; }

bool MinerServer::syncHeld() const {
  return broadcast_.has_value(); // our seal is pending: applying a synced block now would be refused
}

bool MinerServer::syncDue() const {
  // About to lead the next slot: sync before it starts.
  const uint64_t nextSlot = miner_.getCurrentSlot() + 1;
  const int64_t secUntilNextSlot = miner_.getSlotStartTime(nextSlot) - miner_.getConsensusTimestamp();
  return miner_.isSlotLeaderForSlot(nextSlot) && secUntilNextSlot >= 0 && secUntilNextSlot <= SYNC_BEFORE_SLOT_SECONDS;
}

void MinerServer::roleDuties() {
  renewRegistrationPeriodically();
  // Until config is loaded (late joiner waiting for T_CONFIG in synced blocks) only sync; otherwise
  // produce when we lead the slot.
  if (miner_.isConfigReady() && miner_.isSlotLeader()) {
    handleSlotLeaderRole();
  }
}

void MinerServer::fillHealth(Client::NodeHealth &health) const {
  FollowerServer::fillHealth(health);
  health.role = "miner";
  health.blocksSealed = blocksSealed_;
  health.blocksCommitted = blocksCommitted_;
  health.blocksAbandoned = blocksAbandoned_;
  health.lastSealDelayMs = lastSealDelayMs_;
}

MinerServer::Roe<void> MinerServer::verifyGenesisAnchor() {
  const auto &anchor = followerConfig().anchor;
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
  if (auto pinned = NetworkAnchor::verifyGenesisHash<Error>(anchor, calculatedHash);
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

Service::Roe<void> MinerServer::onStart() {
  RunFileConfig runFileConfig;
  auto fields = env_config::commonFields();
  fields.push_back({"PP_MINER_ID", "minerId", env_config::Kind::UInt, "", false});
  fields.push_back({"PP_MINER_KEYS", "keys", env_config::Kind::List, "", /*secret=*/true});
  for (auto &field : FollowerConfig::envFields()) {
    fields.push_back(std::move(field));
  }
  // Nothing here can be guessed (account id, keys, upstream): stop until set.
  auto loaded = loadRunConfig(fields, runFileConfig.toJson(),
                              "set minerId, keys and beacons (upstream multiaddrs), then start again — or set "
                              "PP_MINER_ID, PP_MINER_KEYS (or PP_MINER_KEYS_FILE) and PP_LEDGER_UPSTREAMS");
  if (!loaded) {
    return loaded.error();
  }
  if (auto parsed = runFileConfig.fromJson(loaded.value()); !parsed) {
    return Service::Error(E_CONFIG, "Failed to parse config (config.json and PP_LEDGER_* variables): " +
                                        parsed.error().message);
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

  log().info << "Configuration loaded";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  UDP port: " << runFileConfig.port;
  log().info << "  Beacons: " << runFileConfig.upstreams.size();

  // A miner's network identity is its account's first key (on a host: the host's identity).
  const std::string accountKey = config_.privateKeys.front();
  if (auto started = startFollowing(runFileConfig, [accountKey]() -> Service::Roe<std::string> { return accountKey; });
      !started) {
    return started;
  }

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
  applyTimeOffset(calResult.value());

  // Initialize miner core
  std::filesystem::path minerDataDir =
      std::filesystem::path(getWorkDir()) / DIR_DATA;

  Miner::InitConfig minerConfig;
  minerConfig.minerId = config_.minerId;
  minerConfig.privateKeys = config_.privateKeys;
  minerConfig.timeOffsetMs = clockOffsetMs();
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
  if (learnUpstreamHints()) {
    upstreams_->rerank("the certificates' dial hints"); // the startup sync waits for it
  }

  // Initial catch-up: the same BlockSync as at run time; completions are
  // served here because the run loop has not started yet.
  if (auto error = catchUp(); !error.empty()) {
    return Service::Error(E_ROLE, "Failed to sync blocks from beacon: " + error);
  }

  log().info << "Miner core initialized";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake at init: " << miner_.getStake();

  log().info << "MinerServer initialization complete";
  return {};
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

void MinerServer::initHandlers() {
  // The chain reads are the base's, BLOCK_GET, TX_ADD and EQUIVOCATION_ADD the follower's (bindFollower).
  handle(Client::T_REQ_STATUS, [this](const Client::Request &request) { return hStatus(request); });
  handle(Client::T_REQ_CALIBRATION, [this](const Client::Request &) -> Roe<std::string> {
    if (!miner_.isConfigReady()) {
      return Error(E_REQUEST, "Miner syncing, please retry later");
    }
    return calibrationReply();
  });
}

MinerServer::Roe<std::string>
MinerServer::hStatus(const Client::Request & /*request*/) {
  Client::MinerStatus status;

  status.minerId = config_.minerId;
  status.nextBlockId = miner_.getNextBlockId();
  status.pendingTransactions = miner_.getPendingTransactionCount();
  status.networkId = miner_.getNetworkId();
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
  if (auto dial = dialUpstream(); !dial) {
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
  if (syncInFlight() || broadcast_) {
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
  lastSealDelayMs_ = -miner_.msUntilSlotEnd(block.block.slot);
  startBroadcast(block);
}

void MinerServer::startBroadcast(const Ledger::ChainNode &block) {
  const size_t upstreams = upstreams_->all().size();
  broadcast_ = PendingBroadcast{nextBroadcastId_++, block, BroadcastTally(upstreams)};
  ++blocksSealed_;
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
    ++blocksAbandoned_;
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
    trySyncBlocksFromBeacon(true); // one asked for while the broadcast was pending (syncHeld)
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
  ++blocksCommitted_;
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
  minerInfo.peerId = localPeerId(); // the peer it sends from: the only one a node trusts for it
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
  if (auto dial = dialUpstream(); !dial) {
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

  if (auto dial = dialUpstream(); !dial) {
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
  if (auto verified = admitUpstream(state); !verified) {
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

} // namespace pp
