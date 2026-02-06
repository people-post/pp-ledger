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
  if (!initialized_) {
    return false;
  }

  uint64_t currentSlot = getCurrentSlot();
  return isSlotLeader(currentSlot);
}

bool Miner::isSlotLeader(uint64_t slot) const {
  return getConsensus().isSlotLeader(slot, config_.minerId);
}

bool Miner::isOutOfDate(uint64_t checkpointId) const {
  uint64_t ourBlockId = getNextBlockId();
  
  // If checkpoint is significantly ahead of us (e.g., more than 1000 blocks),
  // we're out of date and should reinit from checkpoint
  if (checkpointId > ourBlockId + 1000) {
    log().info << "Out of date - checkpoint at " << checkpointId 
               << " vs our block " << ourBlockId;
    return true;
  }

  return false;
}

bool Miner::shouldProduceBlock() const {
  if (!initialized_) {
    return false;
  }

  if (!isSlotLeader()) {
    return false;
  }

  uint64_t currentSlot = getCurrentSlot();

  // At most one block per slot
  if (lastProducedSlot_ == currentSlot) {
    return false;
  }

  if (getPendingTransactionCount() == 0) {
    return false;
  }

  // Only produce at end of current slot (within last second of slot)
  if (!getConsensus().isBlockProductionTime(currentSlot)) {
    return false;  // not yet at end of slot
  }

  return true;
}

uint64_t Miner::getStake() const {
  return getConsensus().getStake(config_.minerId);
}

size_t Miner::getPendingTransactionCount() const {
  return pendingTransactions_.size();
}

Miner::Roe<void> Miner::init(const InitConfig &config) {
  if (initialized_) {
    return Error(1, "Miner already initialized");
  }

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
    auto roe = getLedger().mount(ledgerDir);
    if (!roe) {
      return Error(2, "Failed to mount ledger: " + roe.error().message);
    }
    if (getLedger().getNextBlockId() < config.startingBlockId) {
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
    auto ledgerResult = getLedger().init(ledgerConfig);
    if (!ledgerResult) {
      return Error(2, "Failed to initialize ledger: " + ledgerResult.error().message);
    }
  }

  // Initialize consensus
  consensus::Ouroboros::Config cc;
  cc.timeOffset = config.timeOffset;
  getConsensus().init(cc);

  auto loadResult = loadFromLedger(config.startingBlockId);
  if (!loadResult) {
    return Error(2, "Failed to load from ledger: " + loadResult.error().message);
  }

  initialized_ = true;

  log().info << "Miner initialized successfully";
  return {};
}

Miner::Roe<void> Miner::validateBlock(const Ledger::ChainNode& block) const {
  // Call base class implementation
  auto result = Validator::validateBlock(block);
  if (!result) {
    return Error(13, result.error().message);
  }
  return {};
}

Miner::Roe<Ledger::ChainNode> Miner::produceBlock() {
  if (!initialized_) {
    return Error(5, "Miner not initialized");
  }

  if (!isSlotLeader()) {
    return Error(6, "Not slot leader for current slot");
  }

  // Don't produce block if there are no transactions
  if (getPendingTransactionCount() == 0) {
    log().debug << "Skipping block production - no pending transactions";
    return Error(20, "No pending transactions");
  }

  uint64_t currentSlot = getCurrentSlot();
  
  log().info << "Producing block for slot " << currentSlot;

  // Create the block
  auto blockResult = createBlock();
  if (!blockResult) {
    return Error(7, "Failed to create block: " + blockResult.error().message);
  }
  pendingTransactions_.clear();

  return blockResult.value();
}

void Miner::confirmProducedBlock(const Ledger::ChainNode& block) {
  lastProducedBlockId_ = block.block.index;
  lastProducedSlot_ = block.block.slot;

  log().info << "Block produced successfully";
  log().info << "  Block ID: " << block.block.index;
  log().info << "  Slot: " << block.block.slot;
  log().info << "  Transactions: " << block.block.signedTxes.size();
  log().info << "  Hash: " << block.hash;
}

Miner::Roe<void> Miner::addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx) {
  // TODO: Verify signatures

  auto result = addBufferTransaction(bufferBank_, signedTx.obj);
  if (!result) {
    return Error(9, result.error().message);
  }

  pendingTransactions_.push_back(signedTx);
  
  return {};
}

Miner::Roe<void> Miner::addBlock(const Ledger::ChainNode& block) {
  // Adding block is in strict mode if it is at or after the checkpoint id
  bool isStrictMode = block.block.index >= config_.checkpointId;
  // Call base class implementation which validates and adds to chain/ledger
  auto result = Validator::addBlockBase(block, isStrictMode);
  if (!result) {
    return Error(10, result.error().message);
  }

  return {};
}

// Private helper methods

Miner::Roe<Ledger::ChainNode> Miner::createBlock() {
  // Get current slot info
  uint64_t currentSlot = getCurrentSlot();
  auto timestamp = getConsensus().getTimestamp();

  // Get previous block info
  auto latestBlockResult = getLedger().readLastBlock();
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
  block.block.slot = currentSlot;
  block.block.slotLeader = config_.minerId;
  
  // Populate signedTxes
  block.block.signedTxes = pendingTransactions_;

  // Calculate hash
  block.hash = calculateHash(block.block);

  log().debug << "Created block " << blockIndex 
              << " with " << pendingTransactions_.size() << " transactions";

  return block;
}

} // namespace pp
