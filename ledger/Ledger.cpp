#include "Ledger.h"
#include "Block.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {

Ledger::Ledger()
    : Module("ledger"), 
      maxActiveDirSize_(500 * 1024 * 1024) {
}

bool Ledger::hasWallet(const std::string& walletId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ukpWallets_.find(walletId) != ukpWallets_.end();
}

Ledger::Roe<int64_t> Ledger::getBalance(const std::string& walletId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return Ledger::Error(1, "Wallet not found: " + walletId);
    }
    
    return it->second->getBalance();
}

// Transaction operations
Ledger::Roe<void> Ledger::addTransaction(const Transaction& transaction) {
    std::lock_guard<std::mutex> lock(mutex_);
    
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
        pendingTransactions_.push_back(formatTransaction("TRANSFER", transaction.fromWallet, transaction.toWallet, transaction.amount));
    } else {
        // Convert Wallet::Error to Ledger::Error
        return Ledger::Error(result.error().code, result.error().message);
    }
    
    return {}; // Return success
}

void Ledger::clearPendingTransactions() {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingTransactions_.clear();
}

const std::vector<std::string>& Ledger::getPendingTransactions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingTransactions_;
}

size_t Ledger::getPendingTransactionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingTransactions_.size();
}

Ledger::Roe<void> Ledger::commitTransactions() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (pendingTransactions_.empty()) {
        return Ledger::Error(1, "No pending transactions to commit");
    }
    
    try {
        std::string packedData = packTransactions();
        
        // Create a new block with the packed transaction data
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
        
        pendingTransactions_.clear();
        return {};
    } catch (const std::exception& e) {
        return Ledger::Error(2, std::string("Failed to commit transactions: ") + e.what());
    }
}

// IBlockChain interface implementation
std::shared_ptr<iii::Block> Ledger::getLatestBlock() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeBlockDir_) {
        return nullptr;
    }
    // BlockDir returns Block, but IBlockChain interface expects IBlock
    // Block implements IBlock, so we can return it directly
    return activeBlockDir_->getLatestBlock();
}

size_t Ledger::getSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
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

std::string Ledger::packTransactions() const {
    std::stringstream ss;
    ss << "[" << pendingTransactions_.size() << " transactions]\n";
    for (const auto& tx : pendingTransactions_) {
        ss << tx << "\n";
    }
    return ss.str();
}

std::string Ledger::formatTransaction(const std::string& type, const std::string& from, const std::string& to, int64_t amount) {
    std::stringstream ss;
    ss << type << ": " << from << " -> " << to << ": " << amount;
    return ss.str();
}

// Storage management
Ledger::Roe<void> Ledger::initStorage(const StorageConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
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

} // namespace pp
