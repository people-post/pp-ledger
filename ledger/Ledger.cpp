#include "Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "Block.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {

Ledger::Ledger() : Module("ledger"), maxActiveDirSize_(500 * 1024 * 1024) {}

// Initialization
Ledger::Roe<void> Ledger::init(const Config &config) {
  auto result = initStorage(config.storage);
  if (!result) {
    return result.error();
  }

  return {};
}

Ledger::Roe<void> Ledger::initStorage(const StorageConfig &config) {
  if (config.maxActiveDirSize == 0) {
    return Ledger::Error(1, "Max active directory size is not set");
  }

  BlockDir::Config activeCfg(config.activeDirPath, config.blockDirFileSize);
  auto activeRes = activeBlockDir_.init(activeCfg, true);
  if (!activeRes) {
    return Ledger::Error(1, "Failed to initialize active BlockDir");
  }

  // Initialize archive BlockDir
  BlockDir::Config archiveCfg(config.archiveDirPath, config.blockDirFileSize);
  auto archiveRes = archiveBlockDir_.init(archiveCfg);
  if (!archiveRes) {
    return Ledger::Error(2, "Failed to initialize archive BlockDir");
  }

  maxActiveDirSize_ = config.maxActiveDirSize;
  return {};
}

bool Ledger::hasWallet(const std::string &walletId) const {
  return mWallets_.find(walletId) != mWallets_.end();
}

Ledger::Roe<int64_t> Ledger::getBalance(const std::string &walletId) const {
  auto it = mWallets_.find(walletId);
  if (it == mWallets_.end()) {
    return Ledger::Error(1, "Wallet not found: " + walletId);
  }

  return it->second.getBalance();
}

// Transaction operations
Ledger::Roe<void> Ledger::addTransaction(const Transaction &transaction) {
  auto fromIt = mWallets_.find(transaction.fromWallet);
  if (fromIt == mWallets_.end()) {
    return Ledger::Error(1,
                         "Source wallet not found: " + transaction.fromWallet);
  }

  auto toIt = mWallets_.find(transaction.toWallet);
  if (toIt == mWallets_.end()) {
    return Ledger::Error(2, "Destination wallet not found: " +
                                transaction.toWallet);
  }

  auto result = fromIt->second.transfer(toIt->second, transaction.amount);
  if (result.isOk()) {
    blockCache_.transactions.push_back(transaction);
  } else {
    // Convert Wallet::Error to Ledger::Error
    return Ledger::Error(result.error().code, result.error().message);
  }

  return {}; // Return success
}

void Ledger::clearPendingTransactions() {
  blockCache_.transactions.clear();
}

size_t Ledger::getPendingTransactionCount() const {
  return blockCache_.transactions.size();
}

Ledger::Roe<void> Ledger::commitTransactions() {
  if (blockCache_.transactions.empty()) {
    return Ledger::Error(1, "No pending transactions to commit");
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
    return Ledger::Error(5, std::string("Failed to calculate block hash: ") +
                                e.what());
  }

  // Add block to blockchain (managed by activeBlockDir_)
  // This will automatically set the index and write the block to storage
  if (!activeBlockDir_.addBlock(block)) {
    return Ledger::Error(4, "Failed to add block to blockchain");
  }

  // Check if we should transfer blocks to archive
  transferBlocksToArchive();

  blockCache_.transactions.clear();
  return {};
}

// IBlockChain interface implementation
std::shared_ptr<iii::Block> Ledger::getLatestBlock() const {
  // BlockDir returns Block, but IBlockChain interface expects IBlock
  // Block implements IBlock, so we can return it directly
  return activeBlockDir_.getLatestBlock();
}

size_t Ledger::getSize() const {
  return activeBlockDir_.getBlockchainSize();
}

size_t Ledger::getBlockCount() const {
  return activeBlockDir_.getBlockchainSize();
}

bool Ledger::isValid() const {
  return activeBlockDir_.isBlockchainValid();
}

std::shared_ptr<iii::Block> Ledger::getBlock(uint64_t index) const {
  return activeBlockDir_.getBlock(index);
}

void Ledger::transferBlocksToArchive() {
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
std::string Ledger::BlockCache::ltsToString() const {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar &CURRENT_VERSION &transactions;
  return oss.str();
}

bool Ledger::BlockCache::ltsFromString(const std::string &str) {
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
