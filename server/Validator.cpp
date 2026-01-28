#include "Validator.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace pp {

// BlockChain implementation
Validator::BlockChain::BlockChain() {
  // No auto-genesis block - blocks must be added explicitly
}

// Blockchain operations
bool Validator::BlockChain::addBlock(std::shared_ptr<Ledger::ChainNode> block) {
  if (!block) {
    return false;
  }

  chain_.push_back(block);
  return true;
}

std::shared_ptr<Ledger::ChainNode> Validator::BlockChain::getLatestBlock() const {
  if (chain_.empty()) {
    return nullptr;
  }
  return chain_.back();
}

std::shared_ptr<Ledger::ChainNode> Validator::BlockChain::getBlock(uint64_t index) const {
  if (index >= chain_.size()) {
    return nullptr;
  }
  return chain_[index];
}

size_t Validator::BlockChain::getSize() const { return chain_.size(); }

std::vector<std::shared_ptr<Ledger::ChainNode>>
Validator::BlockChain::getBlocks(uint64_t fromIndex, uint64_t toIndex) const {
  std::vector<std::shared_ptr<Ledger::ChainNode>> result;

  if (fromIndex > toIndex || fromIndex >= chain_.size()) {
    return result;
  }

  uint64_t endIndex =
      std::min(toIndex + 1, static_cast<uint64_t>(chain_.size()));
  for (uint64_t i = fromIndex; i < endIndex; i++) {
    result.push_back(chain_[i]);
  }

  return result;
}

std::string Validator::BlockChain::getLastBlockHash() const {
  if (chain_.empty()) {
    return "0";
  }
  return chain_.back()->hash;
}

size_t Validator::BlockChain::trimBlocks(size_t count) {
  if (count == 0 || chain_.empty()) {
    return 0; // Nothing to trim or empty chain
  }

  // Trim from the head (beginning) of the chain
  size_t toRemove = std::min(count, chain_.size());
  chain_.erase(chain_.begin(), chain_.begin() + toRemove);

  return toRemove;
}

Validator::Validator()
    : consensus_(1, 21600) {}

uint64_t Validator::getCurrentBlockId() const {
  // Return the last block ID (nextBlockId - 1)
  uint64_t nextBlockId = ledger_.getNextBlockId();
  return nextBlockId > 0 ? nextBlockId - 1 : 0;
}

Validator::Roe<const Ledger::ChainNode&> Validator::getBlock(uint64_t blockId) const {
  auto spBlock = chain_.getBlock(blockId);
  if (!spBlock) {
    return Error(2, "Block not found: " + std::to_string(blockId));
  }
  return *spBlock;
}

Validator::Roe<void> Validator::addBlockBase(const Ledger::ChainNode& block) {
  // Validate the block first
  auto validationResult = validateBlockBase(block);
  if (!validationResult) {
    return Error(3, "Block validation failed: " + validationResult.error().message);
  }

  // Add to chain
  auto blockPtr = std::make_shared<Ledger::ChainNode>(block);
  if (!chain_.addBlock(blockPtr)) {
    return Error(4, "Failed to add block to chain");
  }

  // Persist to ledger
  auto ledgerResult = ledger_.addBlock(block);
  if (!ledgerResult) {
    return Error(5, "Failed to persist block: " + ledgerResult.error().message);
  }

  log().info << "Block added: " << block.block.index 
             << " from slot leader: " << block.block.slotLeader;

  return {};
}

Validator::Roe<void> Validator::syncChain(const Validator::BlockChain& chain) {
  chain_ = chain;
  return {};
}

Validator::Roe<void> Validator::validateBlockBase(const Ledger::ChainNode& block) const {
  uint64_t slot = block.block.slot;
  std::string slotLeader = block.block.slotLeader;

  // Validate slot leader
  if (!consensus_.validateSlotLeader(slotLeader, slot)) {
    return Error(6, "Invalid slot leader for block at slot " + std::to_string(slot));
  }

  // Validate block timing
  if (!consensus_.validateBlockTiming(block.block.timestamp, slot)) {
    return Error(7, "Block timestamp outside valid slot range");
  }

  // Validate hash chain
  size_t chainSize = chain_.getSize();
  if (chainSize > 0) {
    auto latestBlock = chain_.getLatestBlock();
    if (latestBlock && block.block.previousHash != latestBlock->hash) {
      return Error(8, "Block previous hash does not match chain");
    }

    if (latestBlock && block.block.index != latestBlock->block.index + 1) {
      return Error(9, "Block index mismatch");
    }
  }

  // Validate block hash
  std::string calculatedHash = calculateHash(block.block);
  if (calculatedHash != block.hash) {
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

bool Validator::isChainValid(const BlockChain& chain) const {
  size_t chainSize = chain.getSize();
  
  if (chainSize == 0) {
    return false;
  }

  // Validate all blocks in the chain
  for (size_t i = 0; i < chainSize; i++) {
    const auto &currentBlock = chain.getBlock(i);
    if (!currentBlock) {
      return false;
    }

    // Verify current block's hash
    if (currentBlock->hash != calculateHash(currentBlock->block)) {
      return false;
    }

    // Verify link to previous block (skip for first block if it has special
    // previousHash "0")
    // TODO: Skip validation by index saved in block, not by position in chain
    if (i > 0) {
      const auto &previousBlock = chain.getBlock(i - 1);
      if (!previousBlock) {
        return false;
      }
      if (currentBlock->block.previousHash != previousBlock->hash) {
        return false;
      }
    }
  }

  return true;
}

std::string Validator::calculateHash(const Ledger::Block& block) const {
  // Use ltsToString() to get the serialized block representation
  std::string serialized = block.ltsToString();
  return utl::sha256(serialized);
}

bool Validator::validateBlock(const Ledger::ChainNode& block) const {
  // Verify block's hash
  if (block.hash != calculateHash(block.block)) {
    return false;
  }

  return true;
}

bool Validator::isValidBlockSequence(const Ledger::ChainNode& block) const {
  auto latestBlock = chain_.getLatestBlock();
  
  if (!latestBlock) {
    // First block (genesis)
    return block.block.index == 0;
  }

  // Check index is sequential
  if (block.block.index != latestBlock->block.index + 1) {
    log().warning << "Invalid block index: expected " << (latestBlock->block.index + 1)
                  << " got " << block.block.index;
    return false;
  }

  // Check previous hash matches
  if (block.block.previousHash != latestBlock->hash) {
    log().warning << "Invalid previous hash";
    return false;
  }

  return true;
}

bool Validator::isValidSlotLeader(const Ledger::ChainNode& block) const {
  return consensus_.isSlotLeader(block.block.slot, block.block.slotLeader);
}

bool Validator::isValidTimestamp(const Ledger::ChainNode& block) const {
  int64_t slotStartTime = consensus_.getSlotStartTime(block.block.slot);
  int64_t slotEndTime = slotStartTime + static_cast<int64_t>(consensus_.getSlotDuration());
  
  int64_t blockTime = block.block.timestamp;

  if (blockTime < slotStartTime || blockTime > slotEndTime) {
    log().warning << "Block timestamp out of slot range";
    return false;
  }

  return true;
}

Validator::Roe<void> Validator::processCheckpointTransaction(const Ledger::SignedData<Ledger::Transaction>& signedTx, uint64_t blockId) {
  log().info << "Processing checkpoint transaction in block " << blockId;
  
  // Deserialize BlockChainConfig from transaction metadata
  auto configResult = utl::binaryUnpack<BlockChainConfig>(signedTx.obj.meta);
  if (!configResult) {
    return Error(16, "Failed to deserialize checkpoint config: " + configResult.error().message);
  }

  BlockChainConfig restoredConfig = configResult.value();
  
  // Restore consensus parameters
  consensus_.setGenesisTime(restoredConfig.genesisTime);
  consensus_.setSlotDuration(restoredConfig.slotDuration);
  consensus_.setSlotsPerEpoch(restoredConfig.slotsPerEpoch);
  
  log().info << "Restored BlockChainConfig (version " << BlockChainConfig::VERSION << ")";
  log().info << "  Genesis time: " << restoredConfig.genesisTime;
  log().info << "  Slot duration: " << restoredConfig.slotDuration;
  log().info << "  Slots per epoch: " << restoredConfig.slotsPerEpoch;

  return {};
}

Validator::Roe<void> Validator::processBlock(const Ledger::ChainNode& block, uint64_t blockId) {
  // Process checkpoint transactions to restore BlockChainConfig
  for (const auto& signedTx : block.block.signedTxes) {
    if (signedTx.obj.type == Ledger::Transaction::T_CHECKPOINT) {
      auto processResult = processCheckpointTransaction(signedTx, blockId);
      if (!processResult) {
        log().warning << "Failed to process checkpoint: " << processResult.error().message;
      }
    }
  }
  
  // Add block to in-memory chain
  chain_.addBlock(std::make_shared<Ledger::ChainNode>(block));
  
  return {};
}

Validator::Roe<uint64_t> Validator::mountLedger(const std::string& ledgerPath) {
  log().info << "Mounting ledger at: " << ledgerPath;

  // Mount the ledger
  auto ledgerMountResult = ledger_.mount(ledgerPath);
  if (!ledgerMountResult) {
    return Error(17, "Failed to mount ledger: " + ledgerMountResult.error().message);
  }

  // Process blocks from ledger one by one
  uint64_t blockId = 0;
  uint64_t logInterval = 1000; // Log every 1000 blocks
  while (true) {
    auto blockResult = ledger_.readBlock(blockId);
    if (!blockResult) {
      // No more blocks to read
      break;
    }

    Ledger::ChainNode block = blockResult.value();
    
    // Process the block
    auto processResult = processBlock(block, blockId);
    if (!processResult) {
      return Error(18, "Failed to process block " + std::to_string(blockId) + ": " + processResult.error().message);
    }
    
    blockId++;
    
    // Periodic progress logging
    if (blockId % logInterval == 0) {
      log().info << "Processed " << blockId << " blocks...";
    }
  }

  log().info << "Loaded " << blockId << " blocks from ledger";
  return blockId;
}

} // namespace pp
