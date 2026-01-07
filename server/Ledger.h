#pragma once

#include "BlockChain.h"
#include "Wallet.h"
#include "ResultOrError.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pp {

class Ledger {
public:
    Ledger(uint32_t blockchainDifficulty = 2);
    ~Ledger() = default;
    
    // Wallet management
    ResultOrError<void> createWallet(const std::string& walletId);
    ResultOrError<void> removeWallet(const std::string& walletId);
    bool hasWallet(const std::string& walletId) const;
    ResultOrError<int64_t> getBalance(const std::string& walletId) const;
    
    // Transaction operations
    ResultOrError<void> deposit(const std::string& walletId, int64_t amount);
    ResultOrError<void> withdraw(const std::string& walletId, int64_t amount);
    ResultOrError<void> transfer(const std::string& fromWallet, const std::string& toWallet, int64_t amount);
    
    // Transaction buffer operations
    void addTransaction(const std::string& transaction);
    void clearPendingTransactions();
    const std::vector<std::string>& getPendingTransactions() const;
    size_t getPendingTransactionCount() const;
    
    // Block operations
    ResultOrError<void> commitTransactions();
    
    // BlockChain access
    const BlockChain& getBlockChain() const;
    size_t getBlockCount() const;
    bool isValid() const;
    
private:
    std::string packTransactions() const;
    std::string formatTransaction(const std::string& type, const std::string& from, const std::string& to, int64_t amount);
    
    std::map<std::string, std::unique_ptr<Wallet>> wallets_;
    std::unique_ptr<BlockChain> blockchain_;
    std::vector<std::string> pendingTransactions_;
    mutable std::mutex mutex_;
};

} // namespace pp
