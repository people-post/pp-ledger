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

    struct InitConfig {
        std::string workDir;
        int64_t timeOffset{ 0 };
        uint64_t minerId{ 0 };
        uint64_t startingBlockId{ 0 };
    };

    Miner();
    ~Miner() override = default;

    // ----------------- accessors -------------------------------------
    bool isSlotLeader() const;
    bool isSlotLeader(uint64_t slot) const;
    bool isOutOfDate(uint64_t checkpointId) const;

    bool shouldProduceBlock() const;
    Roe<bool> needsSync(uint64_t remoteBlockId) const;

    uint64_t getStake() const { return getConsensus().getTotalStake(); }
    size_t getPendingTransactionCount() const;

    // ----------------- methods -------------------------------------
    Roe<void> init(const InitConfig &config);

    Roe<void> addTransaction(const Ledger::Transaction &tx);
    Roe<void> addBlock(const Ledger::ChainNode& block);

    Roe<std::shared_ptr<Ledger::ChainNode>> produceBlock();
    Roe<void> validateBlock(const Ledger::ChainNode& block) const;
    void clearTransactionPool();

private:
    constexpr static const char* DIR_LEDGER = "ledger";

    struct Config {
        std::string workDir;
        uint64_t minerId{ 0 };
        BlockChainConfig chain;
    };

    // Helper methods for block production
    Roe<std::shared_ptr<Ledger::ChainNode>> createBlock();
    std::vector<Ledger::Transaction> selectTransactionsForBlock();
    std::string serializeTransactions(const std::vector<Ledger::Transaction>& txs);
    
    // Data cleanup
    void pruneOldBlocks(uint64_t keepFromBlockId);

    // Configuration
    Config config_;

    // Transaction pool
    std::queue<Ledger::Transaction> pendingTransactions_;

    // State tracking
    bool initialized_{ false };
    uint64_t lastProducedBlockId_{ 0 };
};

} // namespace pp

#endif // PP_LEDGER_MINER_H} // namespace pp