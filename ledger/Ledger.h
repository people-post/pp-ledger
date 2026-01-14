#pragma once

#include "Module.h"
#include "BlockChain.h"
#include "Wallet.h"
#include "ResultOrError.hpp"
#include "../interface/BlockChain.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pp {

// Using declaration for interface type
using IBlockChain = iii::BlockChain;

class Ledger : public Module, public IBlockChain {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T>
    using Roe = ResultOrError<T, Error>;
    
    Ledger(uint32_t blockchainDifficulty = 2);
    ~Ledger() = default;
    
    // Wallet management
    Roe<void> createWallet(const std::string& walletId);
    Roe<void> removeWallet(const std::string& walletId);
    bool hasWallet(const std::string& walletId) const;
    Roe<int64_t> getBalance(const std::string& walletId) const;
    
    // Transaction operations
    Roe<void> deposit(const std::string& walletId, int64_t amount);
    Roe<void> withdraw(const std::string& walletId, int64_t amount);
    Roe<void> transfer(const std::string& fromWallet, const std::string& toWallet, int64_t amount);
    
    // Transaction buffer operations
    void addTransaction(const std::string& transaction);
    void clearPendingTransactions();
    const std::vector<std::string>& getPendingTransactions() const;
    size_t getPendingTransactionCount() const;
    
    // Block operations
    Roe<void> commitTransactions();
    
    // IBlockChain interface implementation
    std::shared_ptr<iii::Block> getLatestBlock() const override;
    size_t getSize() const override;
    
    // BlockChain access
    const BlockChain& getBlockChain() const;
    size_t getBlockCount() const;
    bool isValid() const;
    
private:
    std::string packTransactions() const;
    std::string formatTransaction(const std::string& type, const std::string& from, const std::string& to, int64_t amount);
    
    std::map<std::string, std::unique_ptr<Wallet>> ukpWallets_;
    std::unique_ptr<BlockChain> ukpBlockchain_;
    std::vector<std::string> pendingTransactions_;
    mutable std::mutex mutex_;
};

} // namespace pp
