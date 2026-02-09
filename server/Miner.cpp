#include "Miner.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <sstream>

namespace pp {

Miner::Miner() {}

bool Miner::isSlotLeader() const {
  return isStakeholderSlotLeader(config_.minerId, getCurrentSlot());
}

uint64_t Miner::getStake() const {
  return getStakeholderStake(config_.minerId);
}

size_t Miner::getPendingTransactionCount() const {
  return pendingTxes_.size();
}

Miner::Roe<void> Miner::init(const InitConfig &config) {
  config_.workDir = config.workDir;
  config_.minerId = config.minerId;
  config_.privateKey = config.privateKey;
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
    auto roe = mountLedger(ledgerDir);
    if (!roe) {
      return Error(2, "Failed to mount ledger: " + roe.error().message);
    }
    if (getNextBlockId() < config.startingBlockId) {
      log().info << "Ledger data too old, removing existing work directory: " << ledgerDir;
      std::error_code ec;
      std::filesystem::remove_all(ledgerDir, ec);
      if (ec) {
        return Error("Failed to remove existing work directory: " + ec.message());
      }
    }
  }
  
  if (!std::filesystem::exists(ledgerDir)) {
    Ledger::InitConfig ledgerConfig;
    ledgerConfig.workDir = ledgerDir;
    ledgerConfig.startingBlockId = config.startingBlockId;
    auto ledgerResult = initLedger(ledgerConfig);
    if (!ledgerResult) {
      return Error(2, "Failed to initialize ledger: " + ledgerResult.error().message);
    }
  }

  // Initialize consensus
  consensus::Ouroboros::Config consensusConfig;
  consensusConfig.timeOffset = config.timeOffset;
  initConsensus(consensusConfig);

  auto loadResult = loadFromLedger(config.startingBlockId);
  if (!loadResult) {
    return Error(2, "Failed to load from ledger: " + loadResult.error().message);
  }

  log().info << "Miner initialized successfully";
  return {};
}

void Miner::refresh() {
  // Update miner state
  refreshStakeholders();
}

Miner::Roe<bool> Miner::produceBlock(Ledger::ChainNode& block) {
  uint64_t slot = getCurrentSlot();
  // At most one block per slot
  if (lastProducedSlot_ == slot) {
    return false;
  }

  if (slotCache_.slot != slot) {
    // One time initialization of slot cache
    slotCache_ = {};
    slotCache_.slot = slot;
    slotCache_.isLeader = isStakeholderSlotLeader(config_.minerId, slot);
    if (slotCache_.isLeader) {
      // Leader initial evaluations per slot
      slotCache_.txRenewals = collectRenewals(slot);
    }
  }

  if (!slotCache_.isLeader) {
    // Only slot leader can produce block
    return false;
  }

  if (slotCache_.txRenewals.empty() && pendingTxes_.empty()) {
    // No transactions to produce block with
    return false;
  }

  // Only produce at end of current slot (within last second of slot)
  if (!isSlotBlockProductionTime(slot)) {
    return false;
  }

  log().info << "Producing block for slot " << slot;

  // Create the block
  auto createResult = createBlock(slot);
  if (!createResult) {
    return Error(7, "Failed to create block: " + createResult.error().message);
  }
  pendingTxes_.clear();

  block = createResult.value();
  return true;
}

void Miner::markBlockProduction(const Ledger::ChainNode& block) {
  lastProducedBlockId_ = block.block.index;
  lastProducedSlot_ = block.block.slot;
}

Miner::Roe<void> Miner::addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  // TODO: Verify signatures

  auto result = addBufferTransaction(bufferBank_, signedTx.obj);
  if (!result) {
    return Error(9, result.error().message);
  }

  pendingTxes_.push_back(signedTx);
  
  return {};
}

Miner::Roe<void> Miner::addBlock(const Ledger::ChainNode& block) {
  // Adding block is in strict mode if it is at or after the checkpoint id
  bool isStrictMode = block.block.index >= config_.checkpointId;
  // Call base class implementation which validates and adds to chain/ledger
  auto result = Validator::doAddBlock(block, isStrictMode);
  if (!result) {
    return Error(10, result.error().message);
  }

  return {};
}

// Private helper methods

Miner::Roe<Ledger::ChainNode> Miner::createBlock(uint64_t slot) {
  auto timestamp = getConsensusTimestamp();

  // Get previous block info
  auto latestBlockResult = readLastBlock();
  if (!latestBlockResult) {
    return Error(11, "Failed to read latest block: " + latestBlockResult.error().message);
  }
  auto latestBlock = latestBlockResult.value();
  uint64_t blockIndex = latestBlock.block.index + 1;
  std::string previousHash = latestBlock.hash;

  // Create the block
  Ledger::ChainNode block;
  block.block.index = blockIndex;
  block.block.timestamp = timestamp;
  block.block.previousHash = previousHash;
  block.block.slot = slot;
  block.block.slotLeader = config_.minerId;
  
  // Populate signedTxes
  block.block.signedTxes = pendingTxes_;

  // Calculate hash
  block.hash = calculateHash(block.block);

  log().debug << "Created block " << blockIndex 
              << " with " << pendingTxes_.size() << " transactions";

  return block;
}

} // namespace pp
