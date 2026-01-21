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

Beacon::Roe<void> Beacon::init(const Config& config) {
  config_ = config;

  log().info << "Initializing Beacon";

  // Initialize base class
  auto baseResult = initBase(config);
  if (!baseResult) {
    return Error(1, "Failed to initialize base: " + baseResult.error().message);
  }

  log().info << "Beacon initialized successfully";
  log().info << "  Checkpoint min size: " << (config_.checkpointMinSizeBytes / (1024*1024)) << " MB";
  log().info << "  Checkpoint age: " << (config_.checkpointAgeSeconds / (24*3600)) << " days";

  return {};
}

uint64_t Beacon::getCurrentCheckpointId() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return currentCheckpointId_;
}

uint64_t Beacon::getTotalStake() const {
  return consensus_.getTotalStake();
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
  consensus_.registerStakeholder(stakeholder.id, stakeholder.stake);
}

void Beacon::removeStakeholder(const std::string& stakeholderId) {
  {
    std::lock_guard<std::mutex> lock(stakeholdersMutex_);
    stakeholders_.remove_if([&stakeholderId](const Stakeholder& s) {
      return s.id == stakeholderId;
    });
  }

  consensus_.removeStakeholder(stakeholderId);
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

  consensus_.updateStake(stakeholderId, newStake);
  log().info << "Updated stake for " << stakeholderId << ": " << newStake;
}

Beacon::Roe<std::shared_ptr<Block>> Beacon::getBlock(uint64_t blockId) const {
  // Call base class implementation
  auto result = Validator::getBlockBase(blockId);
  if (!result) {
    return Error(2, result.error().message);
  }
  return result.value();
}

Beacon::Roe<std::vector<std::shared_ptr<Block>>> Beacon::getBlocks(
    uint64_t fromId, uint64_t count) const {
  std::vector<std::shared_ptr<Block>> blocks;
  
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t blockId = fromId + i;
    auto block = chain_.getBlock(blockId);
    if (!block) {
      break; // Stop at first missing block
    }
    blocks.push_back(block);
  }

  if (blocks.empty()) {
    return Error(3, "No blocks found starting from: " + std::to_string(fromId));
  }

  return blocks;
}

Beacon::Roe<void> Beacon::addBlock(const Block& block) {
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

Beacon::Roe<void> Beacon::validateBlock(const Block& block) const {
  // Call base class implementation
  auto result = Validator::validateBlockBase(block);
  if (!result) {
    return Error(7, result.error().message);
  }
  return {};
}

Beacon::Roe<void> Beacon::syncChain(const BlockChain& otherChain) {
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

Beacon::Roe<bool> Beacon::shouldAcceptChain(const BlockChain& candidateChain) const {
  // Ouroboros chain selection rule: longest valid chain wins
  if (candidateChain.getSize() > chain_.getSize()) {
    log().info << "Candidate chain is longer: " << candidateChain.getSize() 
               << " vs " << chain_.getSize();
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
  auto result = consensus_.getSlotLeader(slot);
  if (!result) {
    return Error(15, "Failed to get slot leader: " + result.error().message);
  }
  return result.value();
}

// Private helper methods

uint64_t Beacon::calculateBlockchainSize() const {
  // Estimate blockchain size
  // In a real implementation, this would query actual storage
  size_t blockCount = chain_.getSize();
  
  // Rough estimate: ~1KB per block on average
  return blockCount * 1024;
}

uint64_t Beacon::getBlockAge(uint64_t blockId) const {
  auto block = chain_.getBlock(blockId);
  if (!block) {
    return 0;
  }

  auto now = std::chrono::system_clock::now();
  auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();

  int64_t blockTime = block->getTimestamp();
  
  if (currentTime > blockTime) {
    return static_cast<uint64_t>(currentTime - blockTime);
  }

  return 0;
}

Beacon::Roe<void> Beacon::createCheckpoint(uint64_t blockId) {
  std::lock_guard<std::mutex> lock(stateMutex_);

  // Update checkpoint in ledger
  std::vector<uint64_t> checkpoints;
  checkpoints.push_back(blockId);
  
  auto result = ledger_.updateCheckpoints(checkpoints);
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
