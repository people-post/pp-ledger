#include "Ledger.h"
#include "Block.h"
#include "../lib/Serializer.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {

Ledger::Ledger()
    : Module("ledger"), 
      maxActiveDirSize_(500 * 1024 * 1024) {
}

bool Ledger::hasWallet(const std::string& walletId) const {
    return ukpWallets_.find(walletId) != ukpWallets_.end();
}

Ledger::Roe<int64_t> Ledger::getBalance(const std::string& walletId) const {
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return Ledger::Error(1, "Wallet not found: " + walletId);
    }
    
    return it->second->getBalance();
}

// Transaction operations
Ledger::Roe<void> Ledger::addTransaction(const Transaction& transaction) {
    auto fromIt = ukpWallets_.find(transaction.fromWallet);
    if (fromIt == ukpWallets_.end()) {
        return Ledger::Error(1, "Source wallet not found: " + transaction.fromWallet);
    }
    
    auto toIt = ukpWallets_.find(transaction.toWallet);
    if (toIt == ukpWallets_.end()) {
        return Ledger::Error(2, "Destination wallet not found: " + transaction.toWallet);
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

const std::vector<Ledger::Transaction>& Ledger::getPendingTransactions() const {
    return pendingTransactions_.transactions;
}

size_t Ledger::getPendingTransactionCount() const {
    return pendingTransactions_.transactions.size();
}

Ledger::Roe<void> Ledger::commitTransactions() {
    if (pendingTransactions_.transactions.empty()) {
        return Ledger::Error(1, "No pending transactions to commit");
    }
    
    try {
        std::string packedData = pendingTransactions_.ltsToString();
        
        // Create a new block with the serialized transaction data
        if (!activeBlockDir_) {
            return Ledger::Error(3, "Storage not initialized");
        }
        
        auto block = std::make_shared<Block>();
        // Block index will be set automatically by BlockDir::addBlock()
        block->setData(packedData);
        block->setPreviousHash(activeBlockDir_->getLastBlockHash());
        block->setHash(block->calculateHash());
        
        // Add block to blockchain (managed by activeBlockDir_)
        // This will automatically set the index and write the block to storage
        if (!activeBlockDir_->addBlock(block)) {
            return Ledger::Error(4, "Failed to add block to blockchain");
        }
        
        // Check if we should transfer blocks to archive
        transferBlocksToArchive();
        
        pendingTransactions_.transactions.clear();
        return {};
    } catch (const std::exception& e) {
        return Ledger::Error(2, std::string("Failed to commit transactions: ") + e.what());
    }
}

// IBlockChain interface implementation
std::shared_ptr<iii::Block> Ledger::getLatestBlock() const {
    if (!activeBlockDir_) {
        return nullptr;
    }
    // BlockDir returns Block, but IBlockChain interface expects IBlock
    // Block implements IBlock, so we can return it directly
    return activeBlockDir_->getLatestBlock();
}

size_t Ledger::getSize() const {
    if (!activeBlockDir_) {
        return 0;
    }
    return activeBlockDir_->getBlockchainSize();
}

size_t Ledger::getBlockCount() const {
    if (!activeBlockDir_) {
        return 0;
    }
    return activeBlockDir_->getBlockchainSize();
}

bool Ledger::isValid() const {
    if (!activeBlockDir_) {
        return false;
    }
    return activeBlockDir_->isBlockchainValid();
}

std::shared_ptr<iii::Block> Ledger::getBlock(uint64_t index) const {
    if (!activeBlockDir_) {
        return nullptr;
    }
    return activeBlockDir_->getBlock(index);
}

// Storage management
Ledger::Roe<void> Ledger::initStorage(const StorageConfig& config) {
    try {
        // Initialize active BlockDir with blockchain management enabled
        activeBlockDir_ = std::make_unique<BlockDir>();
        BlockDir::Config activeCfg(config.activeDirPath, config.blockDirFileSize);
        auto activeRes = activeBlockDir_->init(activeCfg, true);
        if (!activeRes) {
            return Ledger::Error(1, "Failed to initialize active BlockDir");
        }
        
        // Initialize archive BlockDir
        archiveBlockDir_ = std::make_unique<BlockDir>();
        BlockDir::Config archiveCfg(config.archiveDirPath, config.blockDirFileSize);
        auto archiveRes = archiveBlockDir_->init(archiveCfg);
        if (!archiveRes) {
            return Ledger::Error(2, "Failed to initialize archive BlockDir");
        }
        
        maxActiveDirSize_ = config.maxActiveDirSize;
        return {};
    } catch (const std::exception& e) {
        return Ledger::Error(3, std::string("Failed to initialize storage: ") + e.what());
    }
}

void Ledger::transferBlocksToArchive() {
    if (!activeBlockDir_ || !archiveBlockDir_) {
        return; // Storage not initialized
    }
    
    // Transfer files from active to archive based on storage usage
    while (activeBlockDir_->getTotalStorageSize() >= maxActiveDirSize_) {
        auto result = activeBlockDir_->moveFrontFileTo(*archiveBlockDir_);
        if (!result.isOk()) {
            logging::getLogger("ledger").error << "Failed to move front file to archive";
            break;
        }
    }
}

// PendingTransactions implementation
std::string Ledger::PendingTransactions::ltsToString() const {
    std::ostringstream oss;
    OutputArchive ar(oss);
    // Write version number first
    ar & CURRENT_VERSION;
    // Write transaction data
    ar & transactions;
    return oss.str();
}

bool Ledger::PendingTransactions::ltsFromString(const std::string& str) {
    std::istringstream iss(str);
    InputArchive ar(iss);
    
    // Read version number
    uint32_t version = 0;
    ar & version;
    if (ar.failed()) {
        return false;
    }
    
    // Handle different versions
    switch (version) {
        case 1:
            // Version 1: vector of Transaction objects
            ar & transactions;
            return !ar.failed();
        
        // Future versions can be added here:
        // case 2:
        //     return deserializeVersion2(ar);
        //     break;
        
        default:
            // Unknown version - cannot deserialize
            return false;
    }
}

} // namespace pp
