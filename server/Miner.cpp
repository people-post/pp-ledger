#include "Miner.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
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
  return chain_.isStakeholderSlotLeader(config_.minerId, getCurrentSlot());
}

Miner::Roe<uint64_t> Miner::getSlotLeaderId() const {
  auto result = chain_.getSlotLeader(getCurrentSlot());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return result.value();
}

uint64_t Miner::getStake() const {
  return chain_.getStakeholderStake(config_.minerId);
}

size_t Miner::getPendingTransactionCount() const { return pendingTxes_.size(); }

uint64_t Miner::getNextBlockId() const { return chain_.getNextBlockId(); }

uint64_t Miner::getCurrentSlot() const { return chain_.getCurrentSlot(); }

uint64_t Miner::getCurrentEpoch() const { return chain_.getCurrentEpoch(); }

std::vector<consensus::Stakeholder> Miner::getStakeholders() const {
  return chain_.getStakeholders();
}

Miner::Roe<Ledger::ChainNode> Miner::getBlock(uint64_t blockId) const {
  auto result = chain_.getBlock(blockId);
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

Miner::Roe<void> Miner::init(const InitConfig &config) {
  if (config.privateKeys.empty()) {
    return Error(1, "At least one private key is required");
  }
  config_.workDir = config.workDir;
  config_.minerId = config.minerId;
  config_.privateKeys = config.privateKeys;
  config_.checkpointId = config.checkpointId;

  log().info << "Initializing Miner";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Checkpoint ID: " << config_.checkpointId;

  // Create work directory if it doesn't exist
  if (!std::filesystem::exists(config.workDir)) {
    std::filesystem::create_directories(config.workDir);
  }

  log().info << "  Work directory: " << config.workDir;

  // Initialize ledger
  std::string ledgerDir = config.workDir + "/" + DIR_LEDGER;

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

  // Initialize consensus
  consensus::Ouroboros::Config consensusConfig;
  consensusConfig.timeOffset = config.timeOffset;
  chain_.initConsensus(consensusConfig);

  auto loadResult = chain_.loadFromLedger(config.startingBlockId);
  if (!loadResult) {
    return Error(2,
                 "Failed to load from ledger: " + loadResult.error().message);
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
    slotCache_.txRenewals = renewalsResult.value();
    for (auto &signedTx : slotCache_.txRenewals) {
      auto message = utl::binaryPack(signedTx.obj);
      for (const auto &privateKey : config_.privateKeys) {
        auto result = utl::ed25519Sign(privateKey, message);
        if (!result) {
          slotCache_ = {};
          return Error(12, result.error().message);
        }
        signedTx.signatures.push_back(*result);
      }
      auto addResult =
          chain_.addBufferTransaction(bufferBank_, signedTx, config_.minerId);
      if (!addResult) {
        slotCache_ = {};
        return Error(12, "Failed to add renewal transaction: " +
                             addResult.error().message);
      }
    }
    // Re-apply pending txes so buffer matches block order (renewals then pending)
    for (const auto &signedTx : pendingTxes_) {
      auto addResult =
          chain_.addBufferTransaction(bufferBank_, signedTx, config_.minerId);
      if (!addResult) {
        slotCache_ = {};
        return Error(12, "Failed to add pending transaction: " +
                             addResult.error().message);
      }
    }
  }
  return {};
}

Miner::Roe<bool> Miner::produceBlock(Ledger::ChainNode &block) {
  const uint64_t slot = getCurrentSlot();
  if (lastProducedSlot_ == slot) {
    return false;
  }

  if (slotCache_.slot != slot) {
    auto initResult = initSlotCache(slot);
    if (!initResult) {
      return Error(12, initResult.error().message);
    }
  }
  if (!slotCache_.isLeader) {
    return false;
  }
  if (slotCache_.txRenewals.empty() && pendingTxes_.empty()) {
    return false;
  }

  if (!chain_.isSlotBlockProductionTime(slot)) {
    return false;
  }

  log().info << "Producing block for slot " << slot;

  Miner::BlockTxSet txSet = getBlockTransactionSet();
  auto createResult = createBlock(slot, txSet.signedTxes);
  if (!createResult) {
    return Error(7, "Failed to create block: " + createResult.error().message);
  }

  if (txSet.nPendingIncluded < pendingTxes_.size()) {
    forwardCache_.insert(forwardCache_.end(),
                         pendingTxes_.begin() + txSet.nPendingIncluded,
                         pendingTxes_.end());
  }
  pendingTxes_.clear();

  block = createResult.value();
  return true;
}

void Miner::markBlockProduction(const Ledger::ChainNode &block) {
  lastProducedBlockId_ = block.block.index;
  lastProducedSlot_ = block.block.slot;
}

Miner::Roe<void>
Miner::addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  auto result =
      chain_.addBufferTransaction(bufferBank_, signedTx, config_.minerId);
  if (!result) {
    return Error(9, result.error().message);
  }

  pendingTxes_.push_back(signedTx);

  return {};
}

void Miner::addToForwardCache(
    const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  forwardCache_.push_back(signedTx);
}

std::vector<Ledger::SignedData<Ledger::Transaction>>
Miner::drainForwardCache() {
  std::vector<Ledger::SignedData<Ledger::Transaction>> result;
  result.swap(forwardCache_);
  return result;
}

Miner::Roe<void> Miner::addBlock(const Ledger::ChainNode &block) {
  // Adding block is in strict mode if it is at or after the checkpoint id
  bool isStrictMode = block.block.index >= config_.checkpointId;
  // Call base class implementation which validates and adds to chain/ledger
  auto result = chain_.addBlock(block, isStrictMode);
  if (!result) {
    return Error(10, result.error().message);
  }

  return {};
}

// Private helper methods

Miner::BlockTxSet Miner::getBlockTransactionSet() const {
  Miner::BlockTxSet out;
  const size_t renewalsCount = slotCache_.txRenewals.size();
  const uint64_t maxTx = chain_.getMaxTransactionsPerBlock();
  out.signedTxes = slotCache_.txRenewals;
  if (maxTx == 0) {
    out.nPendingIncluded = pendingTxes_.size();
    out.signedTxes.insert(out.signedTxes.end(), pendingTxes_.begin(),
                          pendingTxes_.end());
  } else {
    const size_t pendingCap =
        (maxTx > renewalsCount) ? static_cast<size_t>(maxTx - renewalsCount)
                               : 0;
    out.nPendingIncluded = std::min(pendingTxes_.size(), pendingCap);
    out.signedTxes.insert(out.signedTxes.end(), pendingTxes_.begin(),
                          pendingTxes_.begin() + out.nPendingIncluded);
  }
  return out;
}

Miner::Roe<Ledger::ChainNode> Miner::createBlock(
    uint64_t slot,
    const std::vector<Ledger::SignedData<Ledger::Transaction>> &signedTxes) {
  auto timestamp = chain_.getConsensusTimestamp();

  auto latestBlockResult = chain_.readLastBlock();
  if (!latestBlockResult) {
    return Error(11, "Failed to read latest block: " +
                         latestBlockResult.error().message);
  }
  auto latestBlock = latestBlockResult.value();
  const uint64_t blockIndex = latestBlock.block.index + 1;

  Ledger::ChainNode block;
  block.block.index = blockIndex;
  block.block.timestamp = timestamp;
  block.block.previousHash = latestBlock.hash;
  block.block.slot = slot;
  block.block.slotLeader = config_.minerId;
  block.block.txIndex =
      latestBlock.block.txIndex + latestBlock.block.signedTxes.size();
  block.block.signedTxes = signedTxes;
  block.hash = calculateHash(block.block);

  log().debug << "Created block " << blockIndex << " with "
              << block.block.signedTxes.size() << " transactions";

  return block;
}

} // namespace pp
