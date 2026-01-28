#include "Miner.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <sstream>

namespace pp {

Miner::Miner() 
    : Validator(),
      initialized_(false),
      lastProducedBlockId_(0) {
  setLogger("Miner");
  log().info << "Miner initialized";
}

Miner::Roe<void> Miner::init(const Config &config) {
  std::lock_guard<std::mutex> lock(getStateMutex());

  if (initialized_) {
    return Error(1, "Miner already initialized");
  }

  config_ = config;

  log().info << "Initializing Miner";
  log().info << "  Miner ID: " << config_.minerId;
  log().info << "  Stake: " << config_.stake;

  // Create work directory if it doesn't exist
  if (!std::filesystem::exists(config.workDir)) {
    std::filesystem::create_directories(config.workDir);
  }

  log().info << "  Work directory: " << config.workDir;

  // Initialize consensus
  getConsensus().setSlotDuration(config.slotDuration);
  getConsensus().setSlotsPerEpoch(config.slotsPerEpoch);
  
  // Set genesis time if not already set
  if (getConsensus().getGenesisTime() == 0) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    getConsensus().setGenesisTime(timestamp);
  }

  // Initialize ledger
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = config.workDir + "/ledger";
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = getLedger().init(ledgerConfig);
  if (!ledgerResult) {
    return Error(2, "Failed to initialize ledger: " + ledgerResult.error().message);
  }

  // Register self as stakeholder
  getConsensus().registerStakeholder(config_.minerId, config_.stake);

  initialized_ = true;

  log().info << "Miner initialized successfully";
  log().info << "  Genesis time: " << getConsensus().getGenesisTime();
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

Miner::Roe<void> Miner::reinitFromCheckpoint(const CheckpointInfo& checkpoint) {
  std::lock_guard<std::mutex> lock(getStateMutex());

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

  {
    std::lock_guard<std::mutex> lock(getStateMutex());
    lastProducedBlockId_ = block->block.index;
  }

  log().info << "Block produced successfully";
  log().info << "  Block ID: " << block->block.index;
  log().info << "  Slot: " << block->block.slot;
  log().info << "  Transactions: " << pendingTransactions_.size();
  log().info << "  Hash: " << block->hash;

  return block;
}

Miner::Roe<void> Miner::addTransaction(const Ledger::Transaction &tx) {
  std::lock_guard<std::mutex> lock(transactionMutex_);

  if (pendingTransactions_.size() >= config_.maxPendingTransactions) {
    return Error(9, "Transaction pool full");
  }

  pendingTransactions_.push(tx);
  
  log().debug << "Transaction added to pool: " << tx.fromWalletId 
              << " -> " << tx.toWalletId << " (" << tx.amount << ")";

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

Miner::Roe<void> Miner::syncChain(const Validator::BlockChain& otherChain) {
  size_t ourSize = getChain().getSize();
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
    oss << ";" << tx.fromWalletId << "," << tx.toWalletId << "," << tx.amount;
  }
  return oss.str();
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
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = config_.workDir + "/ledger";
  ledgerConfig.startingBlockId = startBlockId;

  auto result = getLedger().init(ledgerConfig);
  if (!result) {
    return Error(21, "Failed to reinitialize ledger: " + result.error().message);
  }

  // Prune old blocks from chain that are before checkpoint
  pruneOldBlocks(startBlockId);

  log().info << "Ledger rebuilt successfully";

  return {};
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
