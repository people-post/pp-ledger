#include "Ledger.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {

Ledger::Ledger(uint32_t blockchainDifficulty)
    : Module("ledger"), ukpBlockchain_(std::make_unique<BlockChain>()) {
    if (blockchainDifficulty > 0) {
        ukpBlockchain_->setDifficulty(blockchainDifficulty);
    }
}

// Wallet management
Ledger::Roe<void> Ledger::createWallet(const std::string& walletId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (ukpWallets_.find(walletId) != ukpWallets_.end()) {
        return Ledger::Error(1, "Wallet already exists: " + walletId);
    }
    
    ukpWallets_[walletId] = std::make_unique<Wallet>();
    return {};
}

Ledger::Roe<void> Ledger::removeWallet(const std::string& walletId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return Ledger::Error(1, "Wallet not found: " + walletId);
    }
    
    ukpWallets_.erase(it);
    return {};
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
Ledger::Roe<void> Ledger::deposit(const std::string& walletId, int64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return Ledger::Error(1, "Wallet not found: " + walletId);
    }
    
    auto result = it->second->deposit(amount);
    if (result.isOk()) {
        pendingTransactions_.push_back(formatTransaction("DEPOSIT", "SYSTEM", walletId, amount));
    } else {
        // Convert Wallet::Error to Ledger::Error
        return Ledger::Error(result.error().code, result.error().message);
    }
    
    return {}; // Return success
}

Ledger::Roe<void> Ledger::withdraw(const std::string& walletId, int64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return Ledger::Error(1, "Wallet not found: " + walletId);
    }
    
    auto result = it->second->withdraw(amount);
    if (result.isOk()) {
        pendingTransactions_.push_back(formatTransaction("WITHDRAW", walletId, "SYSTEM", amount));
    } else {
        // Convert Wallet::Error to Ledger::Error
        return Ledger::Error(result.error().code, result.error().message);
    }
    
    return {}; // Return success
}

Ledger::Roe<void> Ledger::transfer(const std::string& fromWallet, const std::string& toWallet, int64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto fromIt = ukpWallets_.find(fromWallet);
    if (fromIt == ukpWallets_.end()) {
        return Ledger::Error(1, "Source wallet not found: " + fromWallet);
    }
    
    auto toIt = ukpWallets_.find(toWallet);
    if (toIt == ukpWallets_.end()) {
        return Ledger::Error(2, "Destination wallet not found: " + toWallet);
    }
    
    auto result = fromIt->second->transfer(*toIt->second, amount);
    if (result.isOk()) {
        pendingTransactions_.push_back(formatTransaction("TRANSFER", fromWallet, toWallet, amount));
    } else {
        // Convert Wallet::Error to Ledger::Error
        return Ledger::Error(result.error().code, result.error().message);
    }
    
    return {}; // Return success
}

void Ledger::addTransaction(const std::string& transaction) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingTransactions_.push_back(transaction);
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
        auto block = std::make_shared<Block>();
        block->setIndex(ukpBlockchain_->getSize());
        block->setData(packedData);
        block->setPreviousHash(ukpBlockchain_->getLastBlockHash());
        block->setHash(block->calculateHash());
        block->mineBlock(ukpBlockchain_->getDifficulty());
        
        ukpBlockchain_->addBlock(block);
        pendingTransactions_.clear();
        return {};
    } catch (const std::exception& e) {
        return Ledger::Error(2, std::string("Failed to commit transactions: ") + e.what());
    }
}

// IBlockChain interface implementation
std::shared_ptr<iii::Block> Ledger::getLatestBlock() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ukpBlockchain_->getLatestBlock();
}

size_t Ledger::getSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ukpBlockchain_->getSize();
}

const BlockChain& Ledger::getBlockChain() const {
    return *ukpBlockchain_;
}

size_t Ledger::getBlockCount() const {
    return ukpBlockchain_->getSize();
}

bool Ledger::isValid() const {
    return ukpBlockchain_->isValid();
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

} // namespace pp
