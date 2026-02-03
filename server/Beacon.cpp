#include "Beacon.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include "../lib/BinaryPack.hpp"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

Beacon::Beacon() {}

bool Beacon::needsCheckpoint() const {
  uint64_t currentSize = getLedger().countSizeFromBlockId(currentCheckpointId_);
  
  // Only evaluate if we have enough data
  return currentSize >= config_.checkpoint.minSizeBytes;
}

uint64_t Beacon::getLastCheckpointId() const {
  return lastCheckpointId_;
}

uint64_t Beacon::getCurrentCheckpointId() const {
  return currentCheckpointId_;
}

Beacon::Roe<std::vector<uint64_t>> Beacon::getCheckpoints() const {
  // This would retrieve checkpoints from ledger
  // For now, return empty vector
  return std::vector<uint64_t>{};
}

Beacon::Roe<uint64_t> Beacon::getSlotLeader(uint64_t slot) const {
  auto result = getConsensus().getSlotLeader(slot);
  if (!result) {
    return Error(15, "Failed to get slot leader: " + result.error().message);
  }
  return result.value();
}

Beacon::Roe<void> Beacon::init(const InitConfig& config) {
  log().info << "Initializing Beacon";
  log().debug << "Init config: " << config;

  // Verify work directory does NOT exist (fresh initialization)
  if (std::filesystem::exists(config.workDir)) {
    return Error("Work directory already exists: " + config.workDir + ". Use mount() to load existing beacon.");
  }

  // Create work directory
  std::filesystem::create_directories(config.workDir);
  log().info << "  Work directory created: " << config.workDir;

  // Initialize consensus
  consensus::Ouroboros::Config cc;
  cc.genesisTime = utl::getCurrentTime();
  cc.timeOffset = 0;
  cc.slotDuration = config.chain.slotDuration;
  cc.slotsPerEpoch = config.chain.slotsPerEpoch;
  getConsensus().init(cc);
  
  // Initialize ledger
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = config.workDir + "/ledger";
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = getLedger().init(ledgerConfig);
  if (!ledgerResult) {
    return Error(2, "Failed to initialize ledger: " + ledgerResult.error().message);
  }

  config_.workDir = config.workDir;
  config_.chain = config.chain;
  config_.chain.genesisTime = cc.genesisTime;

  // Create and add genesis block
  auto genesisBlock = createGenesisBlock(config_.chain);
  
  auto addBlockResult = getLedger().addBlock(genesisBlock);
  if (!addBlockResult) {
    return Error(2, "Failed to add genesis block to ledger: " + addBlockResult.error().message);
  }
  
  log().info << "Genesis block created with checkpoint transaction (version " 
             << SystemCheckpoint::VERSION << ")";

  log().info << "Beacon initialized successfully";
  log().info << "  Genesis time: " << cc.genesisTime;
  log().info << "  Time offset: " << cc.timeOffset;
  log().info << "  Slot duration: " << cc.slotDuration;
  log().info << "  Slots per epoch: " << cc.slotsPerEpoch;
  log().info << "  Max pending transactions: " << config_.chain.maxPendingTransactions;
  log().info << "  Max transactions per block: " << config_.chain.maxTransactionsPerBlock;
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

Beacon::Roe<void> Beacon::mount(const MountConfig& config) {
  log().info << "Mounting Beacon at: " << config.workDir;
  log().debug << "Mount config: " << config;

  // Verify work directory exists (loading existing state)
  if (!std::filesystem::exists(config.workDir)) {
    return Error(3, "Work directory does not exist: " + config.workDir + ". Use init() to create new beacon.");
  }

  config_.workDir = config.workDir;
  config_.checkpoint.minSizeBytes = config.checkpoint.minSizeBytes;
  config_.checkpoint.ageSeconds = config.checkpoint.ageSeconds;

  // Mount the ledger using Validator's mountLedger function
  std::string ledgerPath = config.workDir + "/ledger";
  log().info << "Mounting ledger at: " << ledgerPath;

  // Mount the ledger
  auto ledgerMountResult = getLedger().mount(ledgerPath);
  if (!ledgerMountResult) {
    return Error(3, "Failed to mount ledger: " + ledgerMountResult.error().message);
  }

  auto loadResult = loadFromLedger(0);
  if (!loadResult) {
    return Error(3, "Failed to load data from ledger: " + loadResult.error().message);
  }

  uint64_t blockCount = loadResult.value();

  log().info << "Beacon mounted successfully";
  log().info << "  Loaded " << blockCount << " blocks from ledger";
  log().info << "  Checkpoint min size: " << (config_.checkpoint.minSizeBytes / (1024*1024)) << " MB";
  log().info << "  Checkpoint age: " << (config_.checkpoint.ageSeconds / (24*3600)) << " days";
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

Beacon::Roe<void> Beacon::validateBlock(const Ledger::ChainNode& block) const {
  // Call base class implementation
  auto result = Validator::validateBlockBase(block);
  if (!result) {
    return Error(7, result.error().message);
  }
  return {};
}

Beacon::Roe<void> Beacon::addBlock(const Ledger::ChainNode& block) {
  // Call base class implementation which validates and adds to chain/ledger
  auto result = Validator::addBlockBase(block, false);
  if (!result) {
    return Error(4, result.error().message);
  }

  // Check if we need to evaluate checkpoints
  if (needsCheckpoint()) {
    auto checkpointResult = evaluateCheckpoints();
    if (!checkpointResult) {
      log().warning << "Checkpoint evaluation failed: " << checkpointResult.error().message;
    }
  }

  return {};
}

Beacon::Roe<void> Beacon::evaluateCheckpoints() {
  uint64_t currentSize = getLedger().countSizeFromBlockId(currentCheckpointId_);
  uint64_t nextBlockId = getNextBlockId();

  log().debug << "Evaluating checkpoints - size: " << (currentSize / (1024*1024)) 
              << " MB, next block: " << nextBlockId;

  // Check if we have enough data to consider checkpointing
  if (currentSize < config_.checkpoint.minSizeBytes) {
    log().debug << "Blockchain size below checkpoint threshold";
    return {};
  }

  // Find blocks older than checkpoint age
  uint64_t checkpointAge = config_.checkpoint.ageSeconds;
  std::vector<uint64_t> checkpointCandidates;

  // Iterate through blocks to find old enough blocks
  for (uint64_t blockId = currentCheckpointId_ + 1; blockId < nextBlockId; ++blockId) {
    uint64_t age = getBlockAge(blockId);
    
    if (age >= checkpointAge) {
      checkpointCandidates.push_back(blockId);
    } else {
      break; // Blocks are sequential, so stop when we hit a recent one
    }
  }

  if (checkpointCandidates.empty()) {
    log().debug << "No blocks old enough for checkpointing";
    return {};
  }

  // Create checkpoint at the latest eligible block
  uint64_t newCheckpointId = checkpointCandidates.back();
  auto result = createCheckpoint(newCheckpointId);
  if (!result) {
    return Error(13, "Failed to create checkpoint: " + result.error().message);
  }

  log().info << "Created checkpoint at block " << newCheckpointId;

  return {};
}

// Private helper methods

uint64_t Beacon::getBlockAge(uint64_t blockId) const {
  auto blockResult = getLedger().readBlock(blockId);
  if (!blockResult) {
    return 0;
  }
  auto block = blockResult.value();

  auto now = std::chrono::system_clock::now();
  auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();

  int64_t blockTime = block.block.timestamp;
  
  if (currentTime > blockTime) {
    return static_cast<uint64_t>(currentTime - blockTime);
  }

  return 0;
}

Beacon::Roe<void> Beacon::createCheckpoint(uint64_t blockId) {
  // Update checkpoint in ledger
  std::vector<uint64_t> checkpoints;
  checkpoints.push_back(blockId);
  
  auto result = getLedger().updateCheckpoints(checkpoints);
  if (!result) {
    return Error(14, "Failed to update ledger checkpoints: " + result.error().message);
  }

  // Update current checkpoint
  currentCheckpointId_ = blockId;

  log().info << "Checkpoint created at block " << blockId;
  return {};
}

Ledger::ChainNode Beacon::createGenesisBlock(const BlockChainConfig& config) const {
  log().info << "Creating genesis block";

  SystemCheckpoint systemCheckpoint;
  systemCheckpoint.config = config;
  systemCheckpoint.genesis.balance = 0;
  systemCheckpoint.genesis.publicKey = "";
  systemCheckpoint.genesis.meta = "";

  // Create genesis block with checkpoint transaction containing SystemCheckpoint
  Ledger::ChainNode genesisBlock;
  genesisBlock.block.index = 0;
  genesisBlock.block.timestamp = config.genesisTime;
  genesisBlock.block.previousHash = "0";
  genesisBlock.block.nonce = 0;
  genesisBlock.block.slot = 0;
  genesisBlock.block.slotLeader = 0;

  // TODO: User two transactions or two blocks?

  // Create checkpoint transaction with SystemCheckpoint
  Ledger::Transaction checkpointTx;
  checkpointTx.type = Ledger::Transaction::T_CHECKPOINT;
  checkpointTx.fromWalletId = WID_GENESIS;     // genesis wallet ID
  checkpointTx.toWalletId = WID_TOKEN_RESERVE; // token reserve wallet ID
  checkpointTx.amount = INITIAL_TOKEN_SUPPLY;
  
  // Serialize SystemCheckpoint to transaction metadata
  checkpointTx.meta = systemCheckpoint.ltsToString();

  // Add signed transaction (no signature for genesis)
  Ledger::SignedData<Ledger::Transaction> signedTx;
  signedTx.obj = checkpointTx;
  signedTx.signature = "genesis";
  genesisBlock.block.signedTxes.push_back(signedTx);

  // Calculate hash for genesis block
  genesisBlock.hash = calculateHash(genesisBlock.block);

  log().debug << "Genesis block created with hash: " << genesisBlock.hash;

  return genesisBlock;
}

} // namespace pp
