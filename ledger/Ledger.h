#pragma once

#include "../interface/BlockChain.hpp"
#include "Block.h"
#include "BlockDir.h"
#include "Module.h"
#include "ResultOrError.hpp"
#include "Wallet.h"

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

  template <typename T> using Roe = ResultOrError<T, Error>;

  /**
   * Transaction structure for wallet transfers
   */
  struct Transaction {
    std::string fromWallet; // Source wallet ID
    std::string toWallet;   // Destination wallet ID
    int64_t amount{ 0 };         // Transfer amount

    template <typename Archive> void serialize(Archive &ar) {
      ar &fromWallet &toWallet &amount;
    }
  };

  /**
   * Configuration for Ledger storage
   */
  struct StorageConfig {
    std::string activeDirPath;  // Path for active (hot) blocks
    std::string archiveDirPath; // Path for archived (cold) blocks
    size_t maxActiveDirSize{ 0 }; // Max size of active directory before transferring files (bytes)
    size_t blockDirFileSize{ 0 }; // Max file size for BlockDir files (bytes)
  };

  struct Config {
    StorageConfig storage;
  };

  Ledger();
  ~Ledger() = default;

  /**
   * Initialize storage directories
   * @param config Storage configuration
   * @return true on success, false on error
   */
  Roe<void> init(const Config &config);

  // Wallet management
  bool hasWallet(const std::string &walletId) const;
  Roe<int64_t> getBalance(const std::string &walletId) const;

  // Transaction operations
  Roe<void> addTransaction(const Transaction &transaction);

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
  std::shared_ptr<iii::Block> getBlock(uint64_t index) const;

private:
  /**
   * Private struct to hold pending transactions with long-term storage support
   */
  struct BlockCache {
    std::vector<Transaction> transactions;

    // Serialization version for format evolution
    static constexpr uint32_t CURRENT_VERSION = 1;

    /**
     * Serialize to string for long-term storage (LTS)
     * Format: [version: uint32][data]
     * @return Roe containing serialized binary string representation or error
     */
    std::string ltsToString() const;

    /**
     * Deserialize from string for long-term storage (LTS)
     * Format: [version: uint32][data]
     * @param str Serialized binary string representation
     * @return true on success, false on error
     */
    bool ltsFromString(const std::string &str);
  };

  Roe<void> initStorage(const StorageConfig &config);

  /**
   * Transfer blocks from active to archive directory
   * Called when active directory reaches the transfer threshold
   */
  void transferBlocksToArchive();

  // ------ Private members ------
  std::map<std::string, Wallet> mWallets_;
  BlockCache blockCache_; // Cache of blocks to be committed

  BlockDir activeBlockDir_;  // Hot storage for recent blocks
  BlockDir archiveBlockDir_; // Cold storage for older blocks
  size_t maxActiveDirSize_; // Max size of active directory (bytes)
};

} // namespace pp
