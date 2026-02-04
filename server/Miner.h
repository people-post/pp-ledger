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
        uint64_t checkpointId{ 0 };
        uint64_t tokenId{ AccountBuffer::ID_GENESIS };
        std::string privateKey; // hex-encoded private key
    };

    Miner();
    ~Miner() override = default;

    // ----------------- accessors -------------------------------------
    bool isSlotLeader() const;
    bool isSlotLeader(uint64_t slot) const;
    bool isOutOfDate(uint64_t checkpointId) const;

    bool shouldProduceBlock() const;

    uint64_t getStake() const;
    size_t getPendingTransactionCount() const;

    // ----------------- methods -------------------------------------
    Roe<void> init(const InitConfig &config);

    Roe<void> addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx);
    Roe<void> addBlock(const Ledger::ChainNode& block);

    Roe<Ledger::ChainNode> produceBlock();

private:
    constexpr static const char* DIR_LEDGER = "ledger";

    struct Config {
        std::string workDir;
        uint64_t minerId{ 0 };
        uint64_t tokenId{ AccountBuffer::ID_GENESIS };
        std::string privateKey;  // hex-encoded
        uint64_t checkpointId{ 0 };
        BlockChainConfig chain;
    };

    Roe<void> validateBlock(const Ledger::ChainNode& block) const;
    Roe<Ledger::ChainNode> createBlock();
    
    Config config_;
    AccountBuffer bufferBank_;
    std::vector<Ledger::SignedData<Ledger::Transaction>> pendingTransactions_;
    bool initialized_{ false };
    uint64_t lastProducedBlockId_{ 0 };
    uint64_t lastProducedSlot_{ 0 };  // slot we last produced a block for (at most one per slot)
};

} // namespace pp

#endif // PP_LEDGER_MINER_H} // namespace pp