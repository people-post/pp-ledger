#pragma once

#include "Module.h"
#include "BlockDir.h"
#include "Block.h"
#include "Wallet.h"
#include "ResultOrError.hpp"
#include "../interface/BlockChain.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pp {

// Using declarations for interface types
using IBlockChain = iii::BlockChain;
using IBlock = iii::Block;

class Ledger : public Module, public IBlockChain {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };

    template <typename T>
    using Roe = ResultOrError<T, Error>;
    
    /**
     * Transaction structure for wallet transfers
     */
    struct Transaction {
        std::string fromWallet;  // Source wallet ID
        std::string toWallet;    // Destination wallet ID
        int64_t amount;          // Transfer amount
        
        Transaction() : amount(0) {}
        Transaction(const std::string& from, const std::string& to, int64_t amt)
            : fromWallet(from), toWallet(to), amount(amt) {}

        template <typename Archive>
        void serialize(Archive& ar) {
            ar & fromWallet & toWallet & amount;
        }
    };
    
    /**
     * Configuration for Ledger storage
     */
    struct StorageConfig {
        std::string activeDirPath;      // Path for active (hot) blocks
        std::string archiveDirPath;     // Path for archived (cold) blocks
        size_t maxActiveDirSize;        // Max size of active directory before transferring files (bytes)
        size_t blockDirFileSize;        // Max file size for BlockDir files
        
        StorageConfig() 
            : activeDirPath("./blockchain_active"),
              archiveDirPath("./blockchain_archive"),
              maxActiveDirSize(500 * 1024 * 1024), // 500MB default
              blockDirFileSize(100 * 1024 * 1024) {}
    };
    
    Ledger();
    ~Ledger() = default;
    
    // Wallet management
    bool hasWallet(const std::string& walletId) const;
    Roe<int64_t> getBalance(const std::string& walletId) const;
    
    // Transaction operations
    Roe<void> addTransaction(const Transaction& transaction);
    
    // Transaction buffer operations
    void clearPendingTransactions();
    size_t getPendingTransactionCount() const;
    
    // Block operations
    Roe<void> commitTransactions();
    
    // IBlockChain interface implementation
    std::shared_ptr<iii::Block> getLatestBlock() const override;
    size_t getSize() const override;
    
    // BlockChain access
    size_t getBlockCount() const;
    bool isValid() const;
    
    /**
     * Initialize storage directories
     * @param config Storage configuration
     * @return true on success, false on error
     */
    Roe<void> initStorage(const StorageConfig& config);
    
private:
    /**
     * Transfer blocks from active to archive directory
     * Called when active directory reaches the transfer threshold
     */
    void transferBlocksToArchive();
    
    /**
     * Write block to active storage
     */
    void writeBlockToStorage(std::shared_ptr<IBlock> block);
    std::string packTransactions() const;
    std::string formatTransaction(const std::string& type, const std::string& from, const std::string& to, int64_t amount);
    
    /**
     * Get pending transactions
     */
    const std::vector<std::string>& getPendingTransactions() const;
    
    std::map<std::string, std::unique_ptr<Wallet>> ukpWallets_;
    std::vector<std::string> pendingTransactions_;
    
    // Storage management
    std::unique_ptr<BlockDir> activeBlockDir_;   // Hot storage for recent blocks
    std::unique_ptr<BlockDir> archiveBlockDir_;  // Cold storage for older blocks
    size_t maxActiveDirSize_;                     // Max size of active directory (bytes)
    
    mutable std::mutex mutex_;
};

} // namespace pp
