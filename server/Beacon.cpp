#include "Beacon.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

Beacon::Beacon() 
    : Validator(),
      currentCheckpointId_(0) {
  setLogger("Beacon");
  log().info << "Beacon initialized";
}

Beacon::Roe<void> Beacon::init(const InitConfig& config) {
  log().info << "Initializing Beacon";

  // Verify work directory does NOT exist (fresh initialization)
  if (std::filesystem::exists(config.workDir)) {
    return Error(1, "Work directory already exists: " + config.workDir + ". Use mount() to load existing state.");
  }

  // Create work directory
  std::filesystem::create_directories(config.workDir);
  log().info << "  Work directory created: " << config.workDir;

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

  config_.workDir = config.workDir;
  config_.slotDuration = config.slotDuration;
  config_.slotsPerEpoch = config.slotsPerEpoch;
  config_.checkpointMinSizeBytes = config.checkpointMinSizeBytes;
  config_.checkpointAgeSeconds = config.checkpointAgeSeconds;

  log().info << "Beacon initialized successfully";
  log().info << "  Genesis time: " << getConsensus().getGenesisTime();
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();
  log().info << "  Checkpoint min size: " << (config_.checkpointMinSizeBytes / (1024*1024)) << " MB";
  log().info << "  Checkpoint age: " << (config_.checkpointAgeSeconds / (24*3600)) << " days";

  return {};
}

Beacon::Roe<void> Beacon::mount(const std::string& workDir) {
  log().info << "Mounting Beacon at: " << workDir;

  // Verify work directory exists (loading existing state)
  if (!std::filesystem::exists(workDir)) {
    return Error(3, "Work directory does not exist: " + workDir + ". Use init() to create new beacon.");
  }

  // In a full implementation, this would involve loading existing state
  // from disk, including the ledger, chain, and checkpoints.
  config_.workDir = workDir;

  log().info << "Beacon mounted successfully";

  return {};
}

uint64_t Beacon::getCurrentCheckpointId() const {
  std::lock_guard<std::mutex> lock(getStateMutex());
  return currentCheckpointId_;
}

uint64_t Beacon::getTotalStake() const {
  return getConsensus().getTotalStake();
}

const std::list<Beacon::Stakeholder>& Beacon::getStakeholders() const {
  std::lock_guard<std::mutex> lock(stakeholdersMutex_);
  return stakeholders_;
}

void Beacon::addStakeholder(const Stakeholder& stakeholder) {
  {
    std::lock_guard<std::mutex> lock(stakeholdersMutex_);
    
    // Check if stakeholder already exists
    auto it = std::find_if(stakeholders_.begin(), stakeholders_.end(),
      [&stakeholder](const Stakeholder& s) { return s.id == stakeholder.id; });
    
    if (it != stakeholders_.end()) {
      // Update existing stakeholder
      it->endpoint = stakeholder.endpoint;
      it->stake = stakeholder.stake;
      log().info << "Updated stakeholder: " << stakeholder.id << " stake: " << stakeholder.stake;
    } else {
      // Add new stakeholder
      stakeholders_.push_back(stakeholder);
      log().info << "Added stakeholder: " << stakeholder.id << " stake: " << stakeholder.stake;
    }
  }

  // Register with consensus
  getConsensus().registerStakeholder(stakeholder.id, stakeholder.stake);
}

void Beacon::removeStakeholder(const std::string& stakeholderId) {
  {
    std::lock_guard<std::mutex> lock(stakeholdersMutex_);
    stakeholders_.remove_if([&stakeholderId](const Stakeholder& s) {
      return s.id == stakeholderId;
    });
  }

  getConsensus().removeStakeholder(stakeholderId);
  log().info << "Removed stakeholder: " << stakeholderId;
}

void Beacon::updateStake(const std::string& stakeholderId, uint64_t newStake) {
  {
    std::lock_guard<std::mutex> lock(stakeholdersMutex_);
    auto it = std::find_if(stakeholders_.begin(), stakeholders_.end(),
      [&stakeholderId](const Stakeholder& s) { return s.id == stakeholderId; });
    
    if (it != stakeholders_.end()) {
      it->stake = newStake;
    }
  }

  getConsensus().updateStake(stakeholderId, newStake);
  log().info << "Updated stake for " << stakeholderId << ": " << newStake;
}

Beacon::Roe<void> Beacon::addBlock(const Ledger::ChainNode& block) {
  // Call base class implementation which validates and adds to chain/ledger
  auto result = Validator::addBlockBase(block);
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

Beacon::Roe<void> Beacon::validateBlock(const Ledger::ChainNode& block) const {
  // Call base class implementation
  auto result = Validator::validateBlockBase(block);
  if (!result) {
    return Error(7, result.error().message);
  }
  return {};
}

Beacon::Roe<void> Beacon::syncChain(const Validator::BlockChain& otherChain) {
  // Check if we should accept the other chain
  auto shouldAccept = shouldAcceptChain(otherChain);
  if (!shouldAccept) {
    return Error(12, "Chain evaluation failed: " + shouldAccept.error().message);
  }

  if (!shouldAccept.value()) {
    log().info << "Rejecting chain - current chain is better";
    return {};
  }

  // Accept the new chain
  log().info << "Accepting new chain with " << otherChain.getSize() << " blocks";

  // This is a simplified implementation
  // In production, we would need to:
  // 1. Validate all blocks in the new chain
  // 2. Handle reorganization if needed
  // 3. Update ledger accordingly
  // For now, we just log the action

  return {};
}

Beacon::Roe<bool> Beacon::shouldAcceptChain(const Validator::BlockChain& candidateChain) const {
  // Ouroboros chain selection rule: longest valid chain wins
  if (candidateChain.getSize() > getChain().getSize()) {
    log().info << "Candidate chain is longer: " << candidateChain.getSize() 
               << " vs " << getChain().getSize();
    return true;
  }

  return false;
}

Beacon::Roe<void> Beacon::evaluateCheckpoints() {
  uint64_t currentSize = calculateBlockchainSize();
  uint64_t currentBlock = getCurrentBlockId();

  log().debug << "Evaluating checkpoints - size: " << (currentSize / (1024*1024)) 
              << " MB, current block: " << currentBlock;

  // Check if we have enough data to consider checkpointing
  if (currentSize < config_.checkpointMinSizeBytes) {
    log().debug << "Blockchain size below checkpoint threshold";
    return {};
  }

  // Find blocks older than checkpoint age
  uint64_t checkpointAge = config_.checkpointAgeSeconds;
  std::vector<uint64_t> checkpointCandidates;

  // Iterate through blocks to find old enough blocks
  for (uint64_t blockId = currentCheckpointId_ + 1; blockId < currentBlock; ++blockId) {
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

Beacon::Roe<std::vector<uint64_t>> Beacon::getCheckpoints() const {
  // This would retrieve checkpoints from ledger
  // For now, return empty vector
  return std::vector<uint64_t>{};
}

bool Beacon::needsCheckpoint() const {
  uint64_t currentSize = calculateBlockchainSize();
  
  // Only evaluate if we have enough data
  return currentSize >= config_.checkpointMinSizeBytes;
}

Beacon::Roe<std::string> Beacon::getSlotLeader(uint64_t slot) const {
  auto result = getConsensus().getSlotLeader(slot);
  if (!result) {
    return Error(15, "Failed to get slot leader: " + result.error().message);
  }
  return result.value();
}

// Private helper methods

uint64_t Beacon::calculateBlockchainSize() const {
  // Estimate blockchain size
  // In a real implementation, this would query actual storage
  size_t blockCount = getChain().getSize();
  
  // Rough estimate: ~1KB per block on average
  return blockCount * 1024;
}

uint64_t Beacon::getBlockAge(uint64_t blockId) const {
  auto block = getChain().getBlock(blockId);
  if (!block) {
    return 0;
  }

  auto now = std::chrono::system_clock::now();
  auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();

  int64_t blockTime = block->block.timestamp;
  
  if (currentTime > blockTime) {
    return static_cast<uint64_t>(currentTime - blockTime);
  }

  return 0;
}

Beacon::Roe<void> Beacon::createCheckpoint(uint64_t blockId) {
  std::lock_guard<std::mutex> lock(getStateMutex());

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

  // Optionally prune old data
  auto pruneResult = pruneOldData(blockId);
  if (!pruneResult) {
    log().warning << "Failed to prune old data: " << pruneResult.error().message;
  }

  return {};
}

Beacon::Roe<void> Beacon::pruneOldData(uint64_t checkpointId) {
  // In a full implementation, this would:
  // 1. Identify blocks before the checkpoint
  // 2. Extract essential state (balances, stakes) and append to checkpoint
  // 3. Remove detailed transaction data from old blocks
  // 4. Keep only block headers for chain continuity
  
  log().info << "Pruning data before checkpoint " << checkpointId;
  
  // For now, this is a placeholder
  return {};
}

} // namespace pp
