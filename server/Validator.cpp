#include "Validator.h"
#include "../lib/Logger.h"
#include <chrono>
#include <filesystem>

namespace pp {

Validator::Validator() 
    : consensus_(1, 21600) {
  setLogger("Validator");
}

Validator::Roe<void> Validator::initBase(const BaseConfig& config) {
  baseConfig_ = config;

  // Create work directory if it doesn't exist
  if (!std::filesystem::exists(config.workDir)) {
    std::filesystem::create_directories(config.workDir);
  }

  log().info << "Initializing Validator base";
  log().info << "  Work directory: " << config.workDir;

  // Initialize consensus
  consensus_.setSlotDuration(config.slotDuration);
  consensus_.setSlotsPerEpoch(config.slotsPerEpoch);
  
  // Set genesis time if not already set
  if (consensus_.getGenesisTime() == 0) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    consensus_.setGenesisTime(timestamp);
  }

  // Initialize ledger
  Ledger::Config ledgerConfig;
  ledgerConfig.workDir = config.workDir + "/ledger";
  ledgerConfig.startingBlockId = 0;

  auto ledgerResult = ledger_.init(ledgerConfig);
  if (!ledgerResult) {
    return Error(1, "Failed to initialize ledger: " + ledgerResult.error().message);
  }

  log().info << "Validator base initialized successfully";
  log().info << "  Genesis time: " << consensus_.getGenesisTime();
  log().info << "  Current slot: " << getCurrentSlot();
  log().info << "  Current epoch: " << getCurrentEpoch();

  return {};
}

uint64_t Validator::getCurrentBlockId() const {
  // Return the last block ID (nextBlockId - 1)
  uint64_t nextBlockId = ledger_.getNextBlockId();
  return nextBlockId > 0 ? nextBlockId - 1 : 0;
}

Validator::Roe<const Block&> Validator::getBlock(uint64_t blockId) const {
  auto spBlock = chain_.getBlock(blockId);
  if (!spBlock) {
    return Error(2, "Block not found: " + std::to_string(blockId));
  }
  return *spBlock;
}

Validator::Roe<void> Validator::addBlockBase(const Block& block) {
  // Validate the block first
  auto validationResult = validateBlockBase(block);
  if (!validationResult) {
    return Error(3, "Block validation failed: " + validationResult.error().message);
  }

  // Add to chain
  auto blockPtr = std::make_shared<Block>(block);
  if (!chain_.addBlock(blockPtr)) {
    return Error(4, "Failed to add block to chain");
  }

  // Persist to ledger
  auto ledgerResult = ledger_.addBlock(block);
  if (!ledgerResult) {
    return Error(5, "Failed to persist block: " + ledgerResult.error().message);
  }

  log().info << "Block added: " << block.getIndex() 
             << " from slot leader: " << block.getSlotLeader();

  return {};
}

Validator::Roe<void> Validator::syncChain(const BlockChain& chain) {
  chain_ = chain;
  return {};
}

Validator::Roe<void> Validator::validateBlockBase(const Block& block) const {
  uint64_t slot = block.getSlot();
  std::string slotLeader = block.getSlotLeader();

  // Validate slot leader
  if (!consensus_.validateSlotLeader(slotLeader, slot)) {
    return Error(6, "Invalid slot leader for block at slot " + std::to_string(slot));
  }

  // Validate block timing
  if (!consensus_.validateBlockTiming(block.getTimestamp(), slot)) {
    return Error(7, "Block timestamp outside valid slot range");
  }

  // Validate hash chain
  size_t chainSize = chain_.getSize();
  if (chainSize > 0) {
    auto latestBlock = chain_.getLatestBlock();
    if (latestBlock && block.getPreviousHash() != latestBlock->getHash()) {
      return Error(8, "Block previous hash does not match chain");
    }

    if (latestBlock && block.getIndex() != latestBlock->getIndex() + 1) {
      return Error(9, "Block index mismatch");
    }
  }

  // Validate block hash
  std::string calculatedHash = block.calculateHash();
  if (calculatedHash != block.getHash()) {
    return Error(10, "Block hash validation failed");
  }

  // Validate sequence
  if (!isValidBlockSequence(block)) {
    return Error(11, "Invalid block sequence");
  }

  // Validate slot leader
  if (!isValidSlotLeader(block)) {
    return Error(12, "Invalid slot leader");
  }

  // Validate timestamp
  if (!isValidTimestamp(block)) {
    return Error(13, "Invalid timestamp");
  }

  return {};
}

uint64_t Validator::getCurrentSlot() const {
  return consensus_.getCurrentSlot();
}

uint64_t Validator::getCurrentEpoch() const {
  return consensus_.getCurrentEpoch();
}

bool Validator::isValidBlockSequence(const Block& block) const {
  auto latestBlock = chain_.getLatestBlock();
  
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

bool Validator::isValidSlotLeader(const Block& block) const {
  return consensus_.isSlotLeader(block.getSlot(), block.getSlotLeader());
}

bool Validator::isValidTimestamp(const Block& block) const {
  int64_t slotStartTime = consensus_.getSlotStartTime(block.getSlot());
  int64_t slotEndTime = slotStartTime + static_cast<int64_t>(consensus_.getSlotDuration());
  
  int64_t blockTime = block.getTimestamp();

  if (blockTime < slotStartTime || blockTime > slotEndTime) {
    log().warning << "Block timestamp out of slot range";
    return false;
  }

  return true;
}

} // namespace pp
