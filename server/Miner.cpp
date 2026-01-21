#include "Miner.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <sstream>

namespace pp {

Miner::Miner() 
    : consensus_(1, 21600), 
      initialized_(false),
      lastProducedBlockId_(0) {
  setLogger("Miner");
  log().info << "Miner initialized";
}

Miner::Roe<void> Miner::init(const Config &config) {
  std::lock_guard<std::mutex> lock(stateMutex_);

  if (initialized_) {
    return Error(1, "Miner already initialized");
  }

  config_ = config;

  // Create work directory if it doesn't exist
  if (!std::filesystem::exists(config_.workDir)) {
    std::filesystem::create_directories(config_.workDir);
  }

  log().info << "Initializing Miner";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake: " << config_.stake;
  log().info << "  Work directory: " << config_.workDir;

  // Initialize consensus
  consensus_.setSlotDuration(config_.slotDuration);
  consensus_.setSlotsPerEpoch(config_.slotsPerEpoch);
  
  // Set genesis time if not already set
  if (consensus_.getGenesisTime() == 0) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    consensus_.setGenesisTime(timestamp);
  }

  // Register self as stakeholder
  consensus_.registerStakeholder(config_.minerId, config_.stake);

  // Initialize ledger
  Ledger::Config ledgerConfig;
  ledgerConfig.workDir = config_.workDir + "/ledger";
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = ledger_.init(ledgerConfig);
  if (!ledgerResult) {
    return Error(2, "Failed to initialize ledger: " + ledgerResult.error().message);
  }

  initialized_ = true;

  log().info << "Miner initialized successfully";
  log().info << "  Genesis time: " << consensus_.getGenesisTime();
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

Miner::Roe<void> Miner::reinitFromCheckpoint(const CheckpointInfo& checkpoint) {
  std::lock_guard<std::mutex> lock(stateMutex_);

  log().info << "Reinitializing from checkpoint at block " << checkpoint.blockId;

  // Load checkpoint state
  auto loadResult = loadCheckpoint(checkpoint);
  if (!loadResult) {
    return Error(3, "Failed to load checkpoint: " + loadResult.error().message);
  }

  // Clear existing transaction pool
  {
    std::lock_guard<std::mutex> txLock(transactionMutex_);
    while (!pendingTransactions_.empty()) {
      pendingTransactions_.pop();
    }
  }

  // Rebuild ledger from checkpoint
  auto rebuildResult = rebuildLedgerFromCheckpoint(checkpoint.blockId);
  if (!rebuildResult) {
    return Error(4, "Failed to rebuild ledger: " + rebuildResult.error().message);
  }

  log().info << "Successfully reinitialized from checkpoint";
  log().info << "  Starting from block: " << checkpoint.blockId;
  log().info << "  Current block: " << getCurrentBlockId();

  return {};
}

bool Miner::shouldProduceBlock() const {
  if (!initialized_) {
    return false;
  }

  uint64_t currentSlot = getCurrentSlot();
  return isSlotLeader(currentSlot);
}

Miner::Roe<std::shared_ptr<Block>> Miner::produceBlock() {
  if (!initialized_) {
    return Error(5, "Miner not initialized");
  }

  if (!shouldProduceBlock()) {
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
  if (!chain_.addBlock(block)) {
    return Error(8, "Failed to add produced block to chain");
  }

  // Persist to ledger
  auto ledgerResult = ledger_.addBlock(*block);
  if (!ledgerResult) {
    log().error << "Failed to persist block to ledger: " << ledgerResult.error().message;
    // Don't fail the block production, just log the error
  }

  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastProducedBlockId_ = block->getIndex();
  }

  log().info << "Block produced successfully";
  log().info << "  Block ID: " << block->getIndex();
  log().info << "  Slot: " << block->getSlot();
  log().info << "  Transactions: " << pendingTransactions_.size();
  log().info << "  Hash: " << block->getHash();

  return block;
}

Miner::Roe<void> Miner::addTransaction(const Ledger::Transaction &tx) {
  std::lock_guard<std::mutex> lock(transactionMutex_);

  if (pendingTransactions_.size() >= config_.maxPendingTransactions) {
    return Error(9, "Transaction pool full");
  }

  pendingTransactions_.push(tx);
  
  log().debug << "Transaction added to pool: " << tx.fromWallet 
              << " -> " << tx.toWallet << " (" << tx.amount << ")";

  return {};
}

size_t Miner::getPendingTransactionCount() const {
  std::lock_guard<std::mutex> lock(transactionMutex_);
  return pendingTransactions_.size();
}

void Miner::clearTransactionPool() {
  std::lock_guard<std::mutex> lock(transactionMutex_);
  while (!pendingTransactions_.empty()) {
    pendingTransactions_.pop();
  }
  log().info << "Transaction pool cleared";
}

Miner::Roe<void> Miner::addBlock(const Block& block) {
  // Validate the block first
  auto validationResult = validateBlock(block);
  if (!validationResult) {
    return Error(10, "Block validation failed: " + validationResult.error().message);
  }

  // Add to chain
  auto blockPtr = std::make_shared<Block>(block);
  if (!chain_.addBlock(blockPtr)) {
    return Error(11, "Failed to add block to chain");
  }

  // Persist to ledger
  auto ledgerResult = ledger_.addBlock(block);
  if (!ledgerResult) {
    return Error(12, "Failed to persist block: " + ledgerResult.error().message);
  }

  log().info << "Block added: " << block.getIndex() 
             << " from slot leader: " << block.getSlotLeader();

  return {};
}

Miner::Roe<void> Miner::validateBlock(const Block& block) const {
  // Validate with consensus
  auto consensusResult = consensus_.validateBlock(block, chain_);
  if (!consensusResult) {
    return Error(13, "Consensus validation failed: " + consensusResult.error().message);
  }

  if (!consensusResult.value()) {
    return Error(14, "Block failed consensus validation");
  }

  // Validate sequence
  if (!isValidBlockSequence(block)) {
    return Error(15, "Invalid block sequence");
  }

  // Validate slot leader
  if (!isValidSlotLeader(block)) {
    return Error(16, "Invalid slot leader");
  }

  // Validate timestamp
  if (!isValidTimestamp(block)) {
    return Error(17, "Invalid timestamp");
  }

  return {};
}

uint64_t Miner::getCurrentBlockId() const {
  return ledger_.getCurrentBlockId();
}

Miner::Roe<std::shared_ptr<Block>> Miner::getBlock(uint64_t blockId) const {
  auto block = chain_.getBlock(blockId);
  if (!block) {
    return Error(18, "Block not found: " + std::to_string(blockId));
  }
  return block;
}

Miner::Roe<void> Miner::syncChain(const BlockChain& otherChain) {
  size_t ourSize = chain_.getSize();
  size_t theirSize = otherChain.getSize();

  log().info << "Syncing chain - our size: " << ourSize << " their size: " << theirSize;

  if (theirSize <= ourSize) {
    log().debug << "No sync needed - we are up to date or ahead";
    return {};
  }

  // In a full implementation, we would:
  // 1. Validate the new chain
  // 2. Handle reorganization if needed
  // 3. Request missing blocks
  // 4. Update our chain and ledger

  log().info << "Chain sync would fetch " << (theirSize - ourSize) << " blocks";

  return {};
}

Miner::Roe<bool> Miner::needsSync(uint64_t remoteBlockId) const {
  uint64_t ourBlockId = getCurrentBlockId();
  
  if (remoteBlockId > ourBlockId) {
    log().debug << "Sync needed - remote ahead by " << (remoteBlockId - ourBlockId) << " blocks";
    return true;
  }

  return false;
}

bool Miner::isOutOfDate(uint64_t checkpointId) const {
  uint64_t ourBlockId = getCurrentBlockId();
  
  // If checkpoint is significantly ahead of us (e.g., more than 1000 blocks),
  // we're out of date and should reinit from checkpoint
  if (checkpointId > ourBlockId + 1000) {
    log().info << "Out of date - checkpoint at " << checkpointId 
               << " vs our block " << ourBlockId;
    return true;
  }

  return false;
}

uint64_t Miner::getCurrentSlot() const {
  return consensus_.getCurrentSlot();
}

uint64_t Miner::getCurrentEpoch() const {
  return consensus_.getCurrentEpoch();
}

bool Miner::isSlotLeader(uint64_t slot) const {
  return consensus_.isSlotLeader(slot, config_.minerId);
}

// Private helper methods

Miner::Roe<std::shared_ptr<Block>> Miner::createBlock() {
  // Get current slot info
  uint64_t currentSlot = getCurrentSlot();
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();

  // Get previous block info
  auto latestBlock = chain_.getLatestConcreteBlock();
  uint64_t blockIndex = latestBlock ? latestBlock->getIndex() + 1 : 0;
  std::string previousHash = latestBlock ? latestBlock->getHash() : "";

  // Select transactions
  auto transactions = selectTransactionsForBlock();
  std::string data = serializeTransactions(transactions);

  // Create the block
  auto block = std::make_shared<Block>();
  block->setIndex(blockIndex);
  block->setTimestamp(timestamp);
  block->setData(data);
  block->setPreviousHash(previousHash);
  block->setSlot(currentSlot);
  block->setSlotLeader(config_.minerId);

  // Calculate hash
  block->calculateHash();

  log().debug << "Created block " << blockIndex 
              << " with " << transactions.size() << " transactions";

  return block;
}

std::vector<Ledger::Transaction> Miner::selectTransactionsForBlock() {
  std::lock_guard<std::mutex> lock(transactionMutex_);

  std::vector<Ledger::Transaction> selected;
  size_t maxTxs = std::min(config_.maxTransactionsPerBlock, pendingTransactions_.size());

  for (size_t i = 0; i < maxTxs && !pendingTransactions_.empty(); ++i) {
    selected.push_back(pendingTransactions_.front());
    pendingTransactions_.pop();
  }

  return selected;
}

std::string Miner::serializeTransactions(const std::vector<Ledger::Transaction>& txs) {
  // Simple serialization - in production use proper binary format
  std::ostringstream oss;
  oss << txs.size();
  for (const auto& tx : txs) {
    oss << ";" << tx.fromWallet << "," << tx.toWallet << "," << tx.amount;
  }
  return oss.str();
}

bool Miner::isValidBlockSequence(const Block& block) const {
  auto latestBlock = chain_.getLatestConcreteBlock();
  
  if (!latestBlock) {
    // First block (genesis)
    return block.getIndex() == 0;
  }

  // Check index is sequential
  if (block.getIndex() != latestBlock->getIndex() + 1) {
    log().warning << "Invalid block index: expected " << (latestBlock->getIndex() + 1)
                  << " got " << block.getIndex();
    return false;
  }

  // Check previous hash matches
  if (block.getPreviousHash() != latestBlock->getHash()) {
    log().warning << "Invalid previous hash";
    return false;
  }

  return true;
}

bool Miner::isValidSlotLeader(const Block& block) const {
  return consensus_.isSlotLeader(block.getSlot(), block.getSlotLeader());
}

bool Miner::isValidTimestamp(const Block& block) const {
  int64_t slotStartTime = consensus_.getSlotStartTime(block.getSlot());
  int64_t slotEndTime = slotStartTime + static_cast<int64_t>(consensus_.getSlotDuration());
  
  int64_t blockTime = block.getTimestamp();

  if (blockTime < slotStartTime || blockTime > slotEndTime) {
    log().warning << "Block timestamp out of slot range";
    return false;
  }

  return true;
}

Miner::Roe<void> Miner::loadCheckpoint(const CheckpointInfo& checkpoint) {
  log().info << "Loading checkpoint at block " << checkpoint.blockId;

  // Validate checkpoint
  if (checkpoint.stateData.empty()) {
    return Error(19, "Checkpoint has no state data");
  }

  // Apply checkpoint state
  auto applyResult = applyCheckpointState(checkpoint.stateData);
  if (!applyResult) {
    return Error(20, "Failed to apply checkpoint state: " + applyResult.error().message);
  }

  log().info << "Checkpoint loaded successfully";

  return {};
}

Miner::Roe<void> Miner::applyCheckpointState(const std::vector<uint8_t>& stateData) {
  // In a full implementation, this would:
  // 1. Deserialize the state data
  // 2. Extract balances, stakes, and other state
  // 3. Update ledger with the checkpoint state
  // 4. Initialize chain from checkpoint block
  
  log().info << "Applying checkpoint state (" << stateData.size() << " bytes)";

  // For now, this is a placeholder
  return {};
}

Miner::Roe<void> Miner::rebuildLedgerFromCheckpoint(uint64_t startBlockId) {
  log().info << "Rebuilding ledger from block " << startBlockId;

  // Reinitialize ledger with new starting block ID
  Ledger::Config ledgerConfig;
  ledgerConfig.workDir = config_.workDir + "/ledger";
  ledgerConfig.startingBlockId = startBlockId;

  auto result = ledger_.init(ledgerConfig);
  if (!result) {
    return Error(21, "Failed to reinitialize ledger: " + result.error().message);
  }

  // Prune old blocks from chain that are before checkpoint
  pruneOldBlocks(startBlockId);

  log().info << "Ledger rebuilt successfully";

  return {};
}

void Miner::pruneOldBlocks(uint64_t keepFromBlockId) {
  size_t originalSize = chain_.getSize();
  
  // Calculate how many blocks to trim
  if (keepFromBlockId > 0) {
    size_t blocksToTrim = std::min(static_cast<size_t>(keepFromBlockId), originalSize);
    size_t trimmed = chain_.trimBlocks(blocksToTrim);
    
    log().info << "Pruned " << trimmed << " blocks before checkpoint";
  }
}

} // namespace pp
