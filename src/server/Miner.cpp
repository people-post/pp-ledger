#include "Miner.h"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace pp {

Miner::Miner() {
  redirectLogger("Miner");
  chain_.redirectLogger(log().getFullName() + ".Chain");
}

bool Miner::isSlotLeader() const {
  return chain_.isStakeholderSlotLeader(config_.minerId, getProductionSlot());
}

uint64_t Miner::getProductionSlot() const { return chain_.getProductionSlot(); }

int64_t Miner::msUntilSlotEnd(uint64_t slot) const {
  const int64_t slotEndMs =
      (chain_.getSlotStartTime(slot) + static_cast<int64_t>(chain_.getSlotDuration())) * 1000;
  return slotEndMs - chain_.getConsensusTimestampMs();
}

bool Miner::isSlotLeaderForSlot(uint64_t slot) const {
  return chain_.isStakeholderSlotLeader(config_.minerId, slot);
}

bool Miner::isConfigReady() const { return chain_.isChainConfigReady(); }

Miner::Roe<uint64_t> Miner::getSlotLeaderId() const { return getSlotLeaderIdForSlot(getCurrentSlot()); }

Miner::Roe<uint64_t> Miner::getSlotLeaderIdForSlot(uint64_t slot) const {
  auto result = chain_.getSlotLeader(slot);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

uint64_t Miner::getTipEpoch() const {
  const uint64_t next = chain_.getNextBlockId();
  if (next == 0) {
    return 0;
  }
  auto tip = chain_.readBlock(next - 1);
  return tip ? tip.value().block.epoch : 0;
}

uint64_t Miner::getStake() const {
  return chain_.getStakeholderStake(config_.minerId);
}

size_t Miner::getPendingTransactionCount() const { return pendingTxes_.size(); }

uint64_t Miner::getNextBlockId() const { return chain_.getNextBlockId(); }

uint64_t Miner::getCurrentSlot() const { return chain_.getCurrentSlot(); }

uint64_t Miner::getCurrentEpoch() const { return chain_.getCurrentEpoch(); }

int64_t Miner::getConsensusTimestamp() const {
  return chain_.getConsensusTimestamp();
}

int64_t Miner::getSlotStartTime(uint64_t slot) const {
  return chain_.getSlotStartTime(slot);
}

uint64_t Miner::getSlotDuration() const {
  return chain_.getSlotDuration();
}

std::vector<consensus::Stakeholder> Miner::getStakeholders() const {
  return chain_.getStakeholders();
}

Miner::Roe<Ledger::ChainNode> Miner::readBlock(uint64_t blockId) const {
  auto result = chain_.readBlock(blockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Miner::Roe<Client::UserAccount> Miner::getAccount(uint64_t accountId) const {
  auto result = chain_.getAccount(accountId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

std::string Miner::calculateHash(const Ledger::Block &block) const {
  return chain_.calculateHash(block);
}

Miner::Roe<void> Miner::verifySignedTip(const Client::SignedTip &tip,
                                    const std::vector<const Ledger::Block *> &pending) const {
  auto verified = chain_.verifySignedTip(tip, pending);
  if (!verified) {
    return Error(verified.error().code, verified.error().message);
  }
  return {};
}

Miner::Roe<AuthorityTrail> Miner::authorityTrail() const {
  auto trail = chain_.authorityTrail();
  if (!trail) {
    return Error(trail.error().code, trail.error().message);
  }
  return trail.value();
}

std::optional<std::string> Miner::getTipHash() const {
  const uint64_t next = getNextBlockId();
  if (next == 0) {
    return std::nullopt;
  }
  auto tip = readBlock(next - 1);
  return tip ? std::optional<std::string>(tip.value().hash) : std::nullopt;
}

Miner::Roe<std::vector<Ledger::Record>>
Miner::findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const {
  auto result = chain_.findTransactionsByWalletId(walletId, ioBlockId);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Miner::Roe<Ledger::Record>
Miner::findTransactionByIndex(uint64_t txIndex) const {
  auto result = chain_.findTransactionByIndex(txIndex);
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

Miner::Roe<void> Miner::init(const InitConfig &config) {
  if (config.privateKeys.empty()) {
    return Error(1, "At least one private key is required");
  }
  config_.workDir = config.workDir;
  config_.minerId = config.minerId;
  config_.startingBlockId = config.startingBlockId;
  config_.privateKeys = config.privateKeys;

  log().info << "Initializing Miner";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Starting block ID: " << config_.startingBlockId;

  // Create work directory if it doesn't exist
  if (!std::filesystem::exists(config.workDir)) {
    std::filesystem::create_directories(config.workDir);
  }

  log().info << "  Work directory: " << config.workDir;

  // Initialize consensus(timeOffsetMs only, full config from genesis block when loading)
  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.timeOffsetMs = config.timeOffsetMs;
  chain_.initConsensus(consensusConfig);

  // Initialize ledger
  std::string ledgerDir = config.workDir + "/" + DIR_LEDGER;
  if (auto rewound = applyPendingRewind(ledgerDir); !rewound) {
    return rewound;
  }

  if (std::filesystem::exists(ledgerDir)) {
    auto roe = chain_.mountLedger(ledgerDir);
    if (!roe) {
      return Error(2, "Failed to mount ledger: " + roe.error().message);
    }
    if (getNextBlockId() < config.startingBlockId) {
      log().info << "Ledger data too old, removing existing work directory: "
                 << ledgerDir;
      std::error_code ec;
      std::filesystem::remove_all(ledgerDir, ec);
      if (ec) {
        return Error("Failed to remove existing work directory: " +
                     ec.message());
      }
    }
  }

  if (!std::filesystem::exists(ledgerDir)) {
    Ledger::InitConfig ledgerConfig;
    ledgerConfig.workDir = ledgerDir;
    ledgerConfig.startingBlockId = config.startingBlockId;
    auto ledgerResult = chain_.initLedger(ledgerConfig);
    if (!ledgerResult) {
      return Error(2, "Failed to initialize ledger: " +
                          ledgerResult.error().message);
    }
  }

  auto loadResult = chain_.loadFromLedger(config.startingBlockId);
  if (!loadResult) {
    return Error(2,
                 "Failed to load from ledger: " + loadResult.error().message);
  }

  certificates_ = std::make_unique<CertificateStore>(chain_, config.workDir + "/" + FILE_BEACON_CERT);
  certificates_->redirectLogger(log().getFullName() + ".CertificateStore");
  if (auto loaded = certificates_->load(); !loaded) {
    return Error(2, loaded.error().message);
  }
  equivocations_ = std::make_unique<EquivocationStore>(chain_, config.workDir + "/" + FILE_EQUIVOCATION);
  equivocations_->redirectLogger(log().getFullName() + ".EquivocationStore");
  if (auto loaded = equivocations_->load(); !loaded) {
    return Error(2, loaded.error().message);
  }

  log().info << "Miner initialized successfully";
  return {};
}

void Miner::refresh() {
  // Update miner state
  chain_.refreshStakeholders();
}

Miner::Roe<void> Miner::initSlotCache(uint64_t slot) {
  slotCache_ = {};
  slotCache_.slot = slot;
  slotCache_.isLeader = chain_.isStakeholderSlotLeader(config_.minerId, slot);
  if (slotCache_.isLeader) {
    // Reset buffer so renewals see chain state; pending txes applied after
    bufferBank_.clear();
    // Leader initial evaluations per slot
    auto renewalsResult = chain_.collectRenewals(slot);
    if (!renewalsResult) {
      slotCache_ = {};
      return Error(12, renewalsResult.error().message);
    }
    auto renewals = renewalsResult.value();
    const std::string networkId = chain_.getNetworkId();
    for (auto &rec : renewals) {
      const std::string message = rec.signingMessage(networkId);
      for (const auto &privateKey : config_.privateKeys) {
        auto result = utl::mlDsaSign(privateKey, message);
        if (!result) {
          slotCache_ = {};
          return Error(12, result.error().message);
        }
        rec.signatures.push_back(*result);
      }
      auto addResult =
          chain_.addBufferTransaction(bufferBank_, rec, config_.minerId);
      if (!addResult) {
        slotCache_ = {};
        return Error(12, "Failed to add renewal transaction: " +
                             addResult.error().message);
      }
      slotCache_.leaderRecords.push_back(std::move(rec));
    }
    // The block after a cutover commits the certificate held (bp3): the beacon taking over commits
    // nothing else there. After the renewals: a checkpoint block opens with its genesis renewal.
    if (auto due = chain_.certificateDueAt(chain_.getNextBlockId())) {
      if (auto added = chain_.addBufferTransaction(bufferBank_, *due, config_.minerId); !added) {
        log().error << "The beacon certificate held does not apply to block " << chain_.getNextBlockId() << ": "
                    << added.error().message;
      } else {
        log().info << "Leading slot " << slot << ": block " << chain_.getNextBlockId()
                   << " commits the beacon certificate held (the new beacon's first)";
        slotCache_.leaderRecords.push_back(*due);
      }
    }
    // Re-apply pending txes so buffer matches block order (the leader's own
    // records, then pending). One that no longer applies (included elsewhere, balance
    // spent, window ended) is dropped: aborting here would stall every slot.
    std::vector<Ledger::Record> stillValid;
    stillValid.reserve(pendingTxes_.size());
    for (auto &rec : pendingTxes_) {
      auto addResult = chain_.addBufferTransaction(bufferBank_, rec, config_.minerId);
      if (addResult) {
        stillValid.push_back(std::move(rec));
      } else {
        log().warning << "Dropping pending transaction that no longer applies: "
                      << addResult.error().message;
      }
    }
    pendingTxes_ = std::move(stillValid);
  }
  slotCache_.ready = true;
  return {};
}

Miner::Roe<bool> Miner::shouldAttemptProduction(uint64_t slot,
                                                uint64_t &tipSlotOut) const {
  tipSlotOut = 0;
  // Halted (ba6): no tip of the equivocating generation will cover a block above its height.
  if (const auto &evidence = chain_.equivocation(); evidence && chain_.getNextBlockId() > evidence->height()) {
    return false;
  }
  const bool hasWork =
      !slotCache_.leaderRecords.empty() || !pendingTxes_.empty();
  if (!hasWork) {
    auto tipRoe = chain_.readLastBlock();
    if (!tipRoe) {
      return Error(12, "Failed to read tip for heartbeat: " +
                           tipRoe.error().message);
    }
    tipSlotOut = tipRoe->block.slot;
    // A due miner must show up in a slot it leads, with an empty block if need be.
    if (!shouldSealEmptyHeartbeat(slot, tipSlotOut,
                                  chain_.getHeartbeatSlots()) &&
        !chain_.isMinerDue(config_.minerId, slot)) {
      return false;
    }
  }
  // Seal once the slot has ended, within its delivery window (the leader collects for the whole
  // slot; see SlotCommittee::isSlotSealTime).
  if (!chain_.isSlotSealTime(slot)) {
    return false;
  }
  return true;
}

Miner::Roe<bool> Miner::produceBlock(Ledger::ChainNode &block) {
  const uint64_t slot = getProductionSlot();
  if (lastProducedSlot_ == slot) {
    return false;
  }

  if (!slotCache_.ready || slotCache_.slot != slot) {
    auto initResult = initSlotCache(slot);
    if (!initResult) {
      return Error(12, initResult.error().message);
    }
  }
  if (!slotCache_.isLeader) {
    return false;
  }
  // Started at a checkpoint: no sealing until warmed up and holding the transaction validity
  // window (the seal would be refused anyway).
  if (auto ready = chain_.sealReadiness(slot); !ready) {
    if (sealNotReadyReason_ != ready.error().message) {
      sealNotReadyReason_ = ready.error().message;
      log().info << "Leading slot " << slot << " but not sealing: " << sealNotReadyReason_;
    }
    return false;
  }
  sealNotReadyReason_.clear();

  // Local UX gate only; seal still runs Full admission (incl. heartbeat).
  uint64_t tipSlot = 0;
  auto attempt = shouldAttemptProduction(slot, tipSlot);
  if (!attempt) {
    return attempt;
  }
  if (!attempt.value()) {
    return false;
  }

  const bool hasWork =
      !slotCache_.leaderRecords.empty() || !pendingTxes_.empty();
  if (hasWork) {
    log().info << "Producing block for slot " << slot;
  } else {
    log().info << "Producing heartbeat empty block for slot " << slot
               << " (tip slot " << tipSlot << ")";
  }

  Miner::BlockTxSet txSet = getBlockTransactionSet();
  auto createResult = createBlock(slot, txSet.records);
  if (!createResult) {
    return Error(7, "Failed to create block: " + createResult.error().message);
  }

  // What did not fit stays in the beacon's pool (it drops only included
  // transactions) for the next leader to pull.
  pendingTxes_.clear();

  block = createResult.value();
  return true;
}

void Miner::markBlockProduction(const Ledger::ChainNode &block) {
  lastProducedBlockId_ = block.block.index;
  lastProducedSlot_ = block.block.slot;
}

void Miner::abandonBlock(const Ledger::ChainNode &block) {
  chain_.abandonSeal();
  // The leader's own records (renewals, a certificate due) lead the list and are rebuilt per slot; keep
  // the rest.
  const size_t leaderOwn =
      std::min(slotCache_.leaderRecords.size(), block.block.records.size());
  pendingTxes_.insert(pendingTxes_.begin(),
                      block.block.records.begin() +
                          static_cast<std::ptrdiff_t>(leaderOwn),
                      block.block.records.end());
  // The leader's records and the buffer were built on the abandoned tip; rebuild next time.
  slotCache_ = {};
}

Miner::Roe<void>
Miner::addTransaction(const Ledger::Record &record) {
  // Validate against this slot's buffer rebuilt from the current tip, never a
  // buffer left over from an earlier slot.
  const uint64_t slot = getProductionSlot();
  if (!slotCache_.ready || slotCache_.slot != slot) {
    auto initResult = initSlotCache(slot);
    if (!initResult) {
      return Error(12, initResult.error().message);
    }
  }
  if (!slotCache_.isLeader) {
    return Error(9, "Not slot leader for slot " + std::to_string(slot));
  }
  auto result =
      chain_.addBufferTransaction(bufferBank_, record, config_.minerId);
  if (!result) {
    return Error(9, result.error().message);
  }

  pendingTxes_.push_back(record);

  return {};
}

Miner::Roe<void> Miner::addBlock(const Ledger::ChainNode &block) {
  auto result = chain_.addBlock(block);
  if (!result) {
    return Error(10, result.error().message);
  }
  // The tip moved: this slot's renewals and buffer are stale.
  slotCache_ = {};
  if (certificates_) {
    certificates_->retryPending();
    certificates_->refresh(); // a block may have committed it, or replaced the admins who signed it
  }
  if (equivocations_) {
    equivocations_->retryPending();
    equivocations_->refresh(); // a certificate on chain may have resumed it
  }
  return {};
}

std::optional<Chain::CertificateOffer> Miner::noteCertificate(const Ledger::Record &record,
                                                            const std::string &from) {
  if (!certificates_) {
    return std::nullopt; // no chain yet: nothing to verify it against
  }
  auto offered = certificates_->note(record, from);
  if (offered == Chain::CertificateOffer::Newer && equivocations_) {
    equivocations_->refresh(); // a later generation resumes a halted one
  }
  return offered;
}

std::optional<Chain::EquivocationOffer> Miner::noteEquivocation(const Client::Equivocation &evidence,
                                                                const std::string &from) {
  if (!equivocations_) {
    return std::nullopt; // no chain yet: nothing to verify it against
  }
  return equivocations_->note(evidence, from);
}

// Private helper methods

Miner::BlockTxSet Miner::getBlockTransactionSet() const {
  Miner::BlockTxSet out;
  const size_t leaderCount = slotCache_.leaderRecords.size();
  const uint64_t maxTx = chain_.getMaxTransactionsPerBlock();
  out.records = slotCache_.leaderRecords;
  if (maxTx == 0) {
    out.nPendingIncluded = pendingTxes_.size();
    out.records.insert(out.records.end(), pendingTxes_.begin(),
                          pendingTxes_.end());
  } else {
    const size_t pendingCap =
        (maxTx > leaderCount) ? static_cast<size_t>(maxTx - leaderCount)
                               : 0;
    out.nPendingIncluded = std::min(pendingTxes_.size(), pendingCap);
    out.records.insert(out.records.end(), pendingTxes_.begin(),
                       pendingTxes_.begin() + static_cast<std::ptrdiff_t>(out.nPendingIncluded));
  }
  return out;
}

Miner::Roe<Ledger::ChainNode> Miner::createBlock(
    uint64_t slot,
    const std::vector<Ledger::Record> &records) {
  auto timestamp = chain_.getConsensusTimestamp();

  auto latestBlockResult = chain_.readLastBlock();
  if (!latestBlockResult) {
    return Error(11, "Failed to read latest block: " +
                         latestBlockResult.error().message);
  }
  auto latestBlock = latestBlockResult.value();
  auto block = chain_.linkNextBlock(latestBlock, slot, config_.minerId,
                                    timestamp, records);
  auto sealResult = chain_.sealBlock(block);
  if (!sealResult) {
    return Error(12, "Failed to seal block: " + sealResult.error().message);
  }

  log().debug << "Created block " << block.block.index << " with "
              << block.block.records.size() << " transactions";

  return block;
}


Miner::Roe<void> Miner::applyPendingRewind(const std::string &ledgerDir) {
  auto applied = rewind::applyPendingOrder(config_.workDir, ledgerDir);
  if (!applied) {
    return Error(2, "Failed to apply the pending rewind: " + applied.error().message);
  }
  if (const auto &order = applied.value()) {
    log().error << "Rewound: kept blocks up to " << order->keptUpTo << ", dropped " << order->keptUpTo + 1
                << " to " << order->droppedUpTo << " (past the cutover of beacon generation " << order->generation
                << ")";
  }
  auto last = rewind::lastApplied(config_.workDir);
  if (!last) {
    return Error(2, last.error().message);
  }
  lastRewind_ = last.value();
  return {};
}

Miner::Roe<bool> Miner::scheduleRewind(uint64_t approvedGeneration) {
  auto scheduled = rewind::schedule(chain_, config_.workDir, approvedGeneration, maxAutomaticRewind_);
  if (!scheduled) {
    return Error(2, "Failed to write the rewind order: " + scheduled.error().message);
  }
  const auto &order = scheduled.value().order;
  switch (scheduled.value().kind) {
  case rewind::Scheduled::Kind::None:
    return false;
  case rewind::Scheduled::Kind::HeldBack:
    if (heldBackGeneration_ != order->generation) {
      heldBackGeneration_ = order->generation;
      const auto &depth = scheduled.value().depthSeconds;
      log().error << "Not rewinding on its own: beacon generation " << order->generation << " orders a cut to block "
                  << order->keptUpTo << ", "
                  << (depth ? std::to_string(*depth / 3600) + " h" : std::string("an unknown time"))
                  << " of slots back — more than the " << maxAutomaticRewind_.count() / 3600
                  << " h a node rewinds unattended. Check the certificate; to apply it, restart with "
                  << "--approve-rewind " << order->generation;
    }
    return false;
  case rewind::Scheduled::Kind::Forked:
    if (heldBackGeneration_ != order->generation) {
      heldBackGeneration_ = order->generation;
      log().error << "Not rewinding: beacon generation " << order->generation << " vouches for another block "
                  << order->keptUpTo << " than this node holds — its history forked at or below it, or the "
                  << "certificate names the wrong block. Its tips past the cutover are refused; an operator "
                  << "must look";
    }
    return false;
  case rewind::Scheduled::Kind::Ordered:
    break;
  }
  log().error << "Rewinding to block " << order->keptUpTo << ": blocks " << order->keptUpTo + 1 << " to "
              << order->droppedUpTo << " are past the cutover of beacon generation " << order->generation
              << "; restarting";
  return true;
}

} // namespace pp
