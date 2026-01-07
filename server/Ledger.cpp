#include "Ledger.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pp {

Ledger::Ledger(uint32_t blockchainDifficulty)
    : Module("ledger"), ukpBlockchain_(std::make_unique<BlockChain>(blockchainDifficulty)) {
}

// Wallet management
ResultOrError<void> Ledger::createWallet(const std::string& walletId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (ukpWallets_.find(walletId) != ukpWallets_.end()) {
        return ResultOrError<void>::error("Wallet already exists: " + walletId);
    }
    
    ukpWallets_[walletId] = std::make_unique<Wallet>();
    return ResultOrError<void>();
}

ResultOrError<void> Ledger::removeWallet(const std::string& walletId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return ResultOrError<void>::error("Wallet not found: " + walletId);
    }
    
    ukpWallets_.erase(it);
    return ResultOrError<void>();
}

bool Ledger::hasWallet(const std::string& walletId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ukpWallets_.find(walletId) != ukpWallets_.end();
}

ResultOrError<int64_t> Ledger::getBalance(const std::string& walletId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return ResultOrError<int64_t>::error("Wallet not found: " + walletId);
    }
    
    return ResultOrError<int64_t>(it->second->getBalance());
}

// Transaction operations
ResultOrError<void> Ledger::deposit(const std::string& walletId, int64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return ResultOrError<void>::error("Wallet not found: " + walletId);
    }
    
    auto result = it->second->deposit(amount);
    if (result.isOk()) {
        pendingTransactions_.push_back(formatTransaction("DEPOSIT", "SYSTEM", walletId, amount));
    }
    
    return result;
}

ResultOrError<void> Ledger::withdraw(const std::string& walletId, int64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ukpWallets_.find(walletId);
    if (it == ukpWallets_.end()) {
        return ResultOrError<void>::error("Wallet not found: " + walletId);
    }
    
    auto result = it->second->withdraw(amount);
    if (result.isOk()) {
        pendingTransactions_.push_back(formatTransaction("WITHDRAW", walletId, "SYSTEM", amount));
    }
    
    return result;
}

ResultOrError<void> Ledger::transfer(const std::string& fromWallet, const std::string& toWallet, int64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto fromIt = ukpWallets_.find(fromWallet);
    if (fromIt == ukpWallets_.end()) {
        return ResultOrError<void>::error("Source wallet not found: " + fromWallet);
    }
    
    auto toIt = ukpWallets_.find(toWallet);
    if (toIt == ukpWallets_.end()) {
        return ResultOrError<void>::error("Destination wallet not found: " + toWallet);
    }
    
    auto result = fromIt->second->transfer(*toIt->second, amount);
    if (result.isOk()) {
        pendingTransactions_.push_back(formatTransaction("TRANSFER", fromWallet, toWallet, amount));
    }
    
    return result;
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

ResultOrError<void> Ledger::commitTransactions() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (pendingTransactions_.empty()) {
        return ResultOrError<void>::error("No pending transactions to commit");
    }
    
    try {
        std::string packedData = packTransactions();
        ukpBlockchain_->addBlock(packedData);
        pendingTransactions_.clear();
        return ResultOrError<void>();
    } catch (const std::exception& e) {
        return ResultOrError<void>::error(std::string("Failed to commit transactions: ") + e.what());
    }
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
