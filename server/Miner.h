#ifndef PP_LEDGER_MINER_H
#define PP_LEDGER_MINER_H

#include "Validator.h"
#include "../ledger/Ledger.h"
#include "../consensus/Ouroboros.h"
#include "../network/Types.hpp"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"

#include <string>
#include <cstdint>
#include <vector>
#include <queue>
#include <mutex>
#include <memory>

namespace pp {

/**
 * Miner - Block Producer
 * 
 * Responsibilities:
 * - Produce blocks when selected as slot leader
 * - Maintain local blockchain and ledger state
 * - Process transactions and include them in blocks
 * - Sync with network to get latest blocks
 * - Reinitialize from checkpoints when needed
 * - Validate incoming blocks from other miners
 * 
 * Design:
 * - Miners are the primary block producers in the network
 * - Multiple miners compete to produce blocks based on stake
 * - Can sync from checkpoints to reduce initial sync time
 * - Maintains transaction pool for pending transactions
 * - Uses Ouroboros consensus for slot leader selection
 */
class Miner : public Validator {
public:
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };
    
    template <typename T> using Roe = ResultOrError<T, Error>;

    struct Config {
        // Base configuration
        std::string workDir;
        uint64_t slotDuration = 1; // seconds
        uint64_t slotsPerEpoch = 21600; // ~6 hours
        
        // Miner-specific configuration
        std::string minerId;
        uint64_t stake;
        
        // Transaction pool limits
        size_t maxPendingTransactions = 10000;
        size_t maxTransactionsPerBlock = 1000;
    };

    struct CheckpointInfo {
        uint64_t blockId;
        std::string hash;
        int64_t timestamp;
        std::vector<uint8_t> stateData; // Serialized state snapshot
    };

    Miner();
    ~Miner() override = default;

    // Initialization
    Roe<void> init(const Config &config);
    Roe<void> reinitFromCheckpoint(const CheckpointInfo& checkpoint);

    // Block production
    bool isSlotLeader() const;
    bool shouldProduceBlock() const;
    Roe<std::shared_ptr<Ledger::ChainNode>> produceBlock();
    
    // Transaction management
    Roe<void> addTransaction(const Ledger::Transaction &tx);
    size_t getPendingTransactionCount() const;
    void clearTransactionPool();

    // Block and chain operations
    Roe<void> addBlock(const Ledger::ChainNode& block);
    Roe<void> validateBlock(const Ledger::ChainNode& block) const;

    // Chain synchronization
    Roe<void> syncChain(const Validator::BlockChain& chain);
    Roe<bool> needsSync(uint64_t remoteBlockId) const;
    bool isOutOfDate(uint64_t checkpointId) const;

    // Consensus queries
    bool isSlotLeader(uint64_t slot) const;

    // Status
    std::string getMinerId() const { return config_.minerId; }
    uint64_t getStake() const { return config_.stake; }

private:
    // Helper methods for block production
    Roe<std::shared_ptr<Ledger::ChainNode>> createBlock();
    std::vector<Ledger::Transaction> selectTransactionsForBlock();
    std::string serializeTransactions(const std::vector<Ledger::Transaction>& txs);
    
    // Checkpoint management
    Roe<void> loadCheckpoint(const CheckpointInfo& checkpoint);
    Roe<void> applyCheckpointState(const std::vector<uint8_t>& stateData);
    Roe<void> rebuildLedgerFromCheckpoint(uint64_t startBlockId);
    
    // Data cleanup
    void pruneOldBlocks(uint64_t keepFromBlockId);

    // Configuration
    Config config_;

    // Transaction pool
    std::queue<Ledger::Transaction> pendingTransactions_;
    mutable std::mutex transactionMutex_;

    // State tracking
    bool initialized_;
    uint64_t lastProducedBlockId_;
};

} // namespace pp

#endif // PP_LEDGER_MINER_H} // namespace pp