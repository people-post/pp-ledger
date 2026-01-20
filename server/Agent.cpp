#include "Agent.h"
#include "../lib/BinaryPack.hpp"
#include "../ledger/Block.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {

Agent::Agent() : maxActiveDirSize_(500 * 1024 * 1024) {
  setLogger("agent");
}

// Initialization
Agent::Roe<void> Agent::init(const Config &config) {
  auto result = initStorage(config.storage);
  if (!result) {
    return result.error();
  }

  return {};
}

Agent::Roe<void> Agent::initStorage(const StorageConfig &config) {
  if (config.maxActiveDirSize == 0) {
    return Agent::Error(1, "Max active directory size is not set");
  }

  BlockDir::Config activeCfg(config.activeDirPath, config.blockDirFileSize);
  auto activeRes = activeBlockDir_.init(activeCfg, true);
  if (!activeRes) {
    return Agent::Error(1, "Failed to initialize active BlockDir");
  }

  // Initialize archive BlockDir
  BlockDir::Config archiveCfg(config.archiveDirPath, config.blockDirFileSize);
  auto archiveRes = archiveBlockDir_.init(archiveCfg);
  if (!archiveRes) {
    return Agent::Error(2, "Failed to initialize archive BlockDir");
  }

  maxActiveDirSize_ = config.maxActiveDirSize;
  return {};
}

bool Agent::hasWallet(const std::string &walletId) const {
  return mWallets_.find(walletId) != mWallets_.end();
}

Agent::Roe<int64_t> Agent::getBalance(const std::string &walletId) const {
  auto it = mWallets_.find(walletId);
  if (it == mWallets_.end()) {
    return Agent::Error(1, "Wallet not found: " + walletId);
  }

  return it->second.getBalance();
}

// Transaction operations
Agent::Roe<void> Agent::addTransaction(const Transaction &transaction) {
  auto fromIt = mWallets_.find(transaction.fromWallet);
  if (fromIt == mWallets_.end()) {
    return Agent::Error(1,
                         "Source wallet not found: " + transaction.fromWallet);
  }

  auto toIt = mWallets_.find(transaction.toWallet);
  if (toIt == mWallets_.end()) {
    return Agent::Error(2, "Destination wallet not found: " +
                                transaction.toWallet);
  }

  auto result = fromIt->second.transfer(toIt->second, transaction.amount);
  if (result.isOk()) {
    blockCache_.transactions.push_back(transaction);
  } else {
    // Convert Wallet::Error to Agent::Error
    return Agent::Error(result.error().code, result.error().message);
  }

  return {}; // Return success
}

void Agent::clearPendingTransactions() {
  blockCache_.transactions.clear();
}

size_t Agent::getPendingTransactionCount() const {
  return blockCache_.transactions.size();
}

Agent::Roe<void> Agent::commitTransactions() {
  if (blockCache_.transactions.empty()) {
    return Agent::Error(1, "No pending transactions to commit");
  }

  // Serialize pending transactions
  std::string packedData = blockCache_.ltsToString();

  // Create a new block with the serialized transaction data
  auto block = std::make_shared<Block>();
  // Block index will be set automatically by BlockDir::addBlock()
  block->setData(packedData);
  block->setPreviousHash(activeBlockDir_.getLastBlockHash());
  
  // calculateHash() can throw exceptions from OpenSSL operations
  try {
    block->setHash(block->calculateHash());
  } catch (const std::exception &e) {
    return Agent::Error(5, std::string("Failed to calculate block hash: ") +
                                e.what());
  }

  // Add block to blockchain (managed by activeBlockDir_)
  // This will automatically set the index and write the block to storage
  if (!activeBlockDir_.addBlock(block)) {
    return Agent::Error(4, "Failed to add block to blockchain");
  }

  // Check if we should transfer blocks to archive
  transferBlocksToArchive();

  blockCache_.transactions.clear();
  return {};
}

Agent::Roe<std::string> Agent::produceBlock(
    uint64_t slot, const std::string &slotLeader,
    std::function<Agent::Roe<bool>(const iii::Block &, const IBlockChain &)>
        validator) {
  // Check if there are pending transactions
  if (blockCache_.transactions.empty()) {
    return Agent::Error(1, "No pending transactions to create block");
  }

  // Serialize pending transactions
  std::string packedData = blockCache_.ltsToString();

  // Create a new block with the serialized transaction data
  auto block = std::make_shared<Block>();
  // Block index will be set automatically by BlockDir::addBlock()
  block->setData(packedData);
  block->setPreviousHash(activeBlockDir_.getLastBlockHash());
  block->setSlot(slot);
  block->setSlotLeader(slotLeader);

  // calculateHash() can throw exceptions from OpenSSL operations
  try {
    block->setHash(block->calculateHash());
  } catch (const std::exception &e) {
    return Agent::Error(5, std::string("Failed to calculate block hash: ") +
                                e.what());
  }

  // Validate block using provided validator
  auto validateResult = validator(*block, *this);
  if (!validateResult) {
    return Agent::Error(6, "Block validation failed: " +
                                validateResult.error().message);
  }

  if (!validateResult.value()) {
    return Agent::Error(7, "Block did not pass validation");
  }

  // Add block to blockchain (managed by activeBlockDir_)
  // This will automatically set the index and write the block to storage
  if (!activeBlockDir_.addBlock(block)) {
    return Agent::Error(4, "Failed to add block to blockchain");
  }

  // Check if we should transfer blocks to archive
  transferBlocksToArchive();

  // Serialize the block for broadcasting
  std::string serializedBlock = block->ltsToString();

  blockCache_.transactions.clear();
  return serializedBlock;
}

// IBlockChain interface implementation
std::shared_ptr<iii::Block> Agent::getLatestBlock() const {
  // BlockDir returns Block, but IBlockChain interface expects IBlock
  // Block implements IBlock, so we can return it directly
  return activeBlockDir_.getLatestBlock();
}

size_t Agent::getSize() const {
  return activeBlockDir_.getBlockchainSize();
}

size_t Agent::getBlockCount() const {
  return activeBlockDir_.getBlockchainSize();
}

bool Agent::isValid() const {
  return activeBlockDir_.isBlockchainValid();
}

std::shared_ptr<iii::Block> Agent::getBlock(uint64_t index) const {
  return activeBlockDir_.getBlock(index);
}

Agent::Roe<bool>
Agent::shouldSwitchChain(const IBlockChain &currentChain,
                         const IBlockChain &candidateChain) const {
  // Implement chain selection rule (longest valid chain)
  size_t currentSize = currentChain.getSize();
  size_t candidateSize = candidateChain.getSize();

  if (candidateSize <= currentSize) {
    return false;
  }

  // Validate candidate chain density (not too sparse)
  if (candidateSize > 0) {
    auto latestBlock = candidateChain.getLatestBlock();
    if (latestBlock) {
      uint64_t latestSlot = latestBlock->getSlot();
      // Try to get first slot - if candidateChain is an Agent, we can access getBlock
      // Otherwise, use slot 0 as a fallback
      uint64_t firstSlot = 0;
      const Agent *agentChain = dynamic_cast<const Agent *>(&candidateChain);
      if (agentChain && candidateSize > 0) {
        auto firstBlock = agentChain->getBlock(0);
        if (firstBlock) {
          firstSlot = firstBlock->getSlot();
        }
      }

      if (!validateChainDensity(candidateChain, firstSlot, latestSlot)) {
        return Agent::Error(7, "Candidate chain density too low");
      }
    }
  }

  return true;
}

bool Agent::validateChainDensity(const IBlockChain &chain, uint64_t fromSlot,
                                  uint64_t toSlot) const {
  // Simple density check: at least 50% of slots should have blocks
  // In production, this would be more sophisticated

  if (toSlot <= fromSlot) {
    return true;
  }

  uint64_t slotRange = toSlot - fromSlot + 1;
  size_t blockCount = chain.getSize();

  double density =
      static_cast<double>(blockCount) / static_cast<double>(slotRange);

  return density >= 0.5;
}

void Agent::transferBlocksToArchive() {
  // Transfer files from active to archive based on storage usage
  while (activeBlockDir_.getTotalStorageSize() >= maxActiveDirSize_) {
    auto result = activeBlockDir_.moveFrontFileTo(archiveBlockDir_);
    if (!result.isOk()) {
      log().error << "Failed to move front file to archive";
      break;
    }
  }
}

// BlockCache implementation
std::string Agent::BlockCache::ltsToString() const {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar &CURRENT_VERSION &transactions;
  return oss.str();
}

bool Agent::BlockCache::ltsFromString(const std::string &str) {
  uint32_t version;
  std::istringstream iss(str);
  InputArchive ar(iss);
  ar &version;

  // Handle different versions
  switch (version) {
  case CURRENT_VERSION:
    ar &transactions;
    return true;
  default:
    return false;
  }
}

} // namespace pp
