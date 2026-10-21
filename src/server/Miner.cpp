#include "server/Miner.h"
#include "common/Logger.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"
#include <algorithm>
#include <limits>
#include <chrono>
#include <sstream>

namespace pp {

Miner::Miner() : FollowerCore("Miner") {}

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

int64_t Miner::getConsensusTimestamp() const {
  return chain_.getConsensusTimestamp();
}

int64_t Miner::getSlotStartTime(uint64_t slot) const {
  return chain_.getSlotStartTime(slot);
}

Miner::Roe<void> Miner::init(const InitConfig &config) {
  if (config.privateKeys.empty()) {
    return Error(1, "At least one private key is required");
  }
  config_.minerId = config.minerId;
  config_.startingBlockId = config.startingBlockId;
  config_.privateKeys = config.privateKeys;

  log().info << "Initializing Miner";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Starting block ID: " << config_.startingBlockId;
  if (auto opened = openLedger(config.workDir, config.timeOffsetMs, config.startingBlockId); !opened) {
    return opened;
  }
  log().info << "Miner initialized successfully";
  return {};
}

bool Miner::BlockBudget::take(const Ledger::Record &record) {
  const uint64_t bytes = utl::binaryPack(record).size();
  if (recordsLeft == 0 || bytes > bytesLeft) {
    return false;
  }
  --recordsLeft;
  bytesLeft -= bytes;
  return true;
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
    // Both block limits bind every record (B034, B035): a certificate due first (a takeover's
    // block needs it), then the due renewals while they fit; the rest wait for the next block.
    const uint64_t maxTx = chain_.getMaxTransactionsPerBlock();
    const uint64_t maxBytes = chain_.getMaxBlockBytes();
    const uint64_t shell = maxBytes > 0 ? chain_.estimateBlockShellBytes(slot) : 0;
    slotCache_.budget.recordsLeft = maxTx > 0 ? maxTx : std::numeric_limits<uint64_t>::max();
    slotCache_.budget.bytesLeft = maxBytes == 0 ? std::numeric_limits<uint64_t>::max() : maxBytes > shell ? maxBytes - shell : 0;
    const auto certificate = chain_.certificateDueAt(chain_.getNextBlockId());
    if (certificate && !slotCache_.budget.take(*certificate)) {
      log().error << "The beacon certificate due does not fit block " << chain_.getNextBlockId();
    }
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
      if (!slotCache_.budget.take(rec)) {
        slotCache_.renewalsWait = true;
        log().warning << "Leading slot " << slot << ": " << renewals.size() - slotCache_.leaderRecords.size()
                      << " due renewal(s) wait for the next block (block limits)";
        break;
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
    if (const auto &due = certificate) {
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
  // The committee estimate fell short: drop the last record until the block fits (B034).
  while (!createResult && createResult.error().code == E_BLOCK_TOO_LARGE && txSet.records.size() > 1) {
    txSet.records.pop_back();
    createResult = createBlock(slot, txSet.records);
  }
  // The draft took records the seal refuses (it should not): seal the leader's own records alone
  // rather than leave the slot empty; the pending ones stay in the beacon's pool.
  const size_t leaderOwn = slotCache_.leaderRecords.size();
  if (!createResult && createResult.error().code != E_BLOCK_TOO_LARGE && txSet.records.size() > leaderOwn) {
    log().error << "Sealing slot " << slot << " with " << txSet.records.size() - leaderOwn
                << " pending record(s) failed, sealing without them: " << createResult.error().message;
    txSet.records.resize(leaderOwn);
    createResult = createBlock(slot, txSet.records);
  }
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
  auto added = FollowerCore::addBlock(block);
  if (added) {
    slotCache_ = {}; // the tip moved: this slot's renewals and buffer are stale
  }
  return added;
}

// Private helper methods

Miner::BlockTxSet Miner::getBlockTransactionSet() const {
  Miner::BlockTxSet out;
  out.records = slotCache_.leaderRecords;
  if (slotCache_.renewalsWait) {
    return out; // renewals only while some wait (B035)
  }
  // Pending transactions in order while they fit what the leader's records left (B034).
  BlockBudget budget = slotCache_.budget;
  for (const auto &rec : pendingTxes_) {
    if (!budget.take(rec)) {
      break;
    }
    out.records.push_back(rec);
    ++out.nPendingIncluded;
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
    // The seal assembled the header in place: its size is the block's.
    const uint64_t maxBytes = chain_.getMaxBlockBytes();
    if (maxBytes > 0 && block.block.ltsToString().size() > maxBytes) {
      return Error(E_BLOCK_TOO_LARGE, "Failed to seal block: " + sealResult.error().message);
    }
    return Error(12, "Failed to seal block: " + sealResult.error().message);
  }

  log().debug << "Created block " << block.block.index << " with "
              << block.block.records.size() << " transactions";

  return block;
}

} // namespace pp
