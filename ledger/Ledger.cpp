#include "Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "Block.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {

Ledger::Ledger() : Module("ledger"), maxActiveDirSize_(500 * 1024 * 1024) {}

bool Ledger::hasWallet(const std::string &walletId) const {
  return ukpWallets_.find(walletId) != ukpWallets_.end();
}

Ledger::Roe<int64_t> Ledger::getBalance(const std::string &walletId) const {
  auto it = ukpWallets_.find(walletId);
  if (it == ukpWallets_.end()) {
    return Ledger::Error(1, "Wallet not found: " + walletId);
  }

  return it->second->getBalance();
}

// Transaction operations
Ledger::Roe<void> Ledger::addTransaction(const Transaction &transaction) {
  auto fromIt = ukpWallets_.find(transaction.fromWallet);
  if (fromIt == ukpWallets_.end()) {
    return Ledger::Error(1,
                         "Source wallet not found: " + transaction.fromWallet);
  }

  auto toIt = ukpWallets_.find(transaction.toWallet);
  if (toIt == ukpWallets_.end()) {
    return Ledger::Error(2, "Destination wallet not found: " +
                                transaction.toWallet);
  }

  auto result = fromIt->second->transfer(*toIt->second, transaction.amount);
  if (result.isOk()) {
    pendingTransactions_.transactions.push_back(transaction);
  } else {
    // Convert Wallet::Error to Ledger::Error
    return Ledger::Error(result.error().code, result.error().message);
  }

  return {}; // Return success
}

void Ledger::clearPendingTransactions() {
  pendingTransactions_.transactions.clear();
}

const std::vector<Ledger::Transaction> &Ledger::getPendingTransactions() const {
  return pendingTransactions_.transactions;
}

size_t Ledger::getPendingTransactionCount() const {
  return pendingTransactions_.transactions.size();
}

Ledger::Roe<void> Ledger::commitTransactions() {
  if (pendingTransactions_.transactions.empty()) {
    return Ledger::Error(1, "No pending transactions to commit");
  }

  // Serialize pending transactions
  auto packedDataResult = pendingTransactions_.ltsToString();
  if (!packedDataResult) {
    return Ledger::Error(2, "Failed to serialize pending transactions: " +
                                packedDataResult.error().message);
  }

  // Create a new block with the serialized transaction data
  auto block = std::make_shared<Block>();
  // Block index will be set automatically by BlockDir::addBlock()
  block->setData(packedDataResult.value());
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

  pendingTransactions_.transactions.clear();
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

// Storage management
Ledger::Roe<void> Ledger::initStorage(const StorageConfig &config) {
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

// PendingTransactions implementation
// Helper struct for versioned serialization
struct VersionedPendingTransactions {
  uint32_t version;
  std::vector<Ledger::Transaction> transactions;

  template <typename Archive> void serialize(Archive &ar) {
    ar &version;
    ar &transactions;
  }
};

Ledger::Roe<std::string> Ledger::PendingTransactions::ltsToString() const {
  try {
    VersionedPendingTransactions versioned;
    versioned.version = CURRENT_VERSION;
    versioned.transactions = transactions;
    std::string result = utl::binaryPack(versioned);
    return result;
  } catch (const std::exception &e) {
    return Ledger::Error(1, std::string("Failed to serialize pending transactions: ") + e.what());
  }
}

bool Ledger::PendingTransactions::ltsFromString(const std::string &str) {
  auto result = utl::binaryUnpack<VersionedPendingTransactions>(str);
  if (result.isError()) {
    return false;
  }

  const auto &versioned = result.value();

  // Handle different versions
  switch (versioned.version) {
  case 1:
    // Version 1: vector of Transaction objects
    transactions = versioned.transactions;
    return true;

    // Future versions can be added here:
    // case 2:
    //     return handleVersion2(versioned);
    //     break;

  default:
    // Unknown version - cannot deserialize
    return false;
  }
}

} // namespace pp
