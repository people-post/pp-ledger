#include "Miner.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <sstream>

namespace pp {

Miner::Miner() {}

Miner::Roe<void> Miner::init(const InitConfig &config) {
  if (initialized_) {
    return Error(1, "Miner already initialized");
  }

  config_.workDir = config.workDir;
  config_.minerId = config.minerId;

  log().info << "Initializing Miner";
  log().info << "  Miner ID: " << config_.minerId;

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

bool Miner::isSlotLeader() const {
  if (!initialized_) {
    return false;
  }

  uint64_t currentSlot = getCurrentSlot();
  return isSlotLeader(currentSlot);
}

bool Miner::shouldProduceBlock() const {
  return isSlotLeader() && getPendingTransactionCount() > 0;
}

Miner::Roe<std::shared_ptr<Ledger::ChainNode>> Miner::produceBlock() {
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

  auto block = blockResult.value();

  // Add to our chain
  if (!getChainMutable().addBlock(block)) {
    return Error(8, "Failed to add produced block to chain");
  }

  // Persist to ledger
  auto ledgerResult = getLedger().addBlock(*block);
  if (!ledgerResult) {
    log().error << "Failed to persist block to ledger: " << ledgerResult.error().message;
    // Don't fail the block production, just log the error
  }

  lastProducedBlockId_ = block->block.index;

  log().info << "Block produced successfully";
  log().info << "  Block ID: " << block->block.index;
  log().info << "  Slot: " << block->block.slot;
  log().info << "  Transactions: " << pendingTransactions_.size();
  log().info << "  Hash: " << block->hash;

  return block;
}

Miner::Roe<void> Miner::addTransaction(const Ledger::Transaction &tx) {
  /* TODO: update code after loadFromLedger can read maxPendingTransactions 
  if (pendingTransactions_.size() >= config_.maxPendingTransactions) {
    return Error(9, "Transaction pool full");
  }
  */

  pendingTransactions_.push(tx);
  
  log().debug << "Transaction added to pool: " << tx.fromWalletId 
              << " -> " << tx.toWalletId << " (" << tx.amount << ")";

  return {};
}

size_t Miner::getPendingTransactionCount() const {
  return pendingTransactions_.size();
}

void Miner::clearTransactionPool() {
  while (!pendingTransactions_.empty()) {
    pendingTransactions_.pop();
  }
  log().info << "Transaction pool cleared";
}

Miner::Roe<void> Miner::addBlock(const Ledger::ChainNode& block) {
  // Call base class implementation which validates and adds to chain/ledger
  auto result = Validator::addBlockBase(block);
  if (!result) {
    return Error(10, result.error().message);
  }

  return {};
}

Miner::Roe<void> Miner::validateBlock(const Ledger::ChainNode& block) const {
  // Call base class implementation
  auto result = Validator::validateBlockBase(block);
  if (!result) {
    return Error(13, result.error().message);
  }
  return {};
}

Miner::Roe<bool> Miner::needsSync(uint64_t remoteBlockId) const {
  uint64_t ourBlockId = getNextBlockId();
  
  if (remoteBlockId > ourBlockId) {
    log().debug << "Sync needed - remote ahead by " << (remoteBlockId - ourBlockId) << " blocks";
    return true;
  }

  return false;
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

bool Miner::isSlotLeader(uint64_t slot) const {
  return getConsensus().isSlotLeader(slot, config_.minerId);
}

// Private helper methods

Miner::Roe<std::shared_ptr<Ledger::ChainNode>> Miner::createBlock() {
  // Get current slot info
  uint64_t currentSlot = getCurrentSlot();
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();

  // Get previous block info
  auto latestBlock = getChain().getLatestBlock();
  uint64_t blockIndex = latestBlock ? latestBlock->block.index + 1 : 0;
  std::string previousHash = latestBlock ? latestBlock->hash : "";

  // Select transactions and convert to SignedData
  auto transactions = selectTransactionsForBlock();

  // Create the block
  auto block = std::make_shared<Ledger::ChainNode>();
  block->block.index = blockIndex;
  block->block.timestamp = timestamp;
  block->block.previousHash = previousHash;
  block->block.slot = currentSlot;
  block->block.slotLeader = config_.minerId;
  
  // Populate signedTxes
  block->block.signedTxes.clear();
  for (const auto& tx : transactions) {
    Ledger::SignedData<Ledger::Transaction> signedTx;
    signedTx.obj = tx;
    signedTx.signature = ""; // Signature will be set when transaction is signed
    block->block.signedTxes.push_back(signedTx);
  }

  // Calculate hash
  block->hash = calculateHash(block->block);

  log().debug << "Created block " << blockIndex 
              << " with " << transactions.size() << " transactions";

  return block;
}

std::vector<Ledger::Transaction> Miner::selectTransactionsForBlock() {
  std::vector<Ledger::Transaction> selected;
  /*
  // TODO: Revaluate later
  size_t maxTxs = std::min(config_.maxTransactionsPerBlock, pendingTransactions_.size());

  for (size_t i = 0; i < maxTxs && !pendingTransactions_.empty(); ++i) {
    selected.push_back(pendingTransactions_.front());
    pendingTransactions_.pop();
  }
  */
  return selected;
}

std::string Miner::serializeTransactions(const std::vector<Ledger::Transaction>& txs) {
  // Simple serialization - in production use proper binary format
  std::ostringstream oss;
  oss << txs.size();
  for (const auto& tx : txs) {
    oss << ";" << tx.fromWalletId << "," << tx.toWalletId << "," << tx.amount;
  }
  return oss.str();
}

void Miner::pruneOldBlocks(uint64_t keepFromBlockId) {
  size_t originalSize = getChain().getSize();
  
  // Calculate how many blocks to trim
  if (keepFromBlockId > 0) {
    size_t blocksToTrim = std::min(static_cast<size_t>(keepFromBlockId), originalSize);
    size_t trimmed = getChainMutable().trimBlocks(blocksToTrim);
    
    log().info << "Pruned " << trimmed << " blocks before checkpoint";
  }
}

} // namespace pp
