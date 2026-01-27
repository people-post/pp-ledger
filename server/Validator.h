#ifndef PP_LEDGER_VALIDATOR_H
#define PP_LEDGER_VALIDATOR_H

#include "../ledger/Ledger.h"
#include "../consensus/Ouroboros.h"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"

#include <string>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace pp {

/**
 * Validator - Base class for block validators (Miner and Beacon)
 * 
 * Provides common functionality for:
 * - Block validation
 * - Chain management
 * - Consensus integration
 * - Ledger operations
 */
class Validator : public Module {
public:
    /**
     * Concrete implementation of BlockChain data structure
     *
     * Manages an in-memory chain of blocks.
     */
    class BlockChain {
    public:
      BlockChain();
      ~BlockChain() = default;

      // Blockchain operations
      std::shared_ptr<Ledger::ChainNode> getLatestBlock() const;
      size_t getSize() const;

      // Additional blockchain operations
      bool addBlock(std::shared_ptr<Ledger::ChainNode> block);
      std::shared_ptr<Ledger::ChainNode> getBlock(uint64_t index) const;
      std::string getLastBlockHash() const;

      /**
       * Trim blocks from the head of the chain
       * Removes the first n blocks from the beginning of the chain
       * @param count Number of blocks to trim from the head
       * @return Number of blocks removed
       */
      size_t trimBlocks(size_t count);

    private:
      // Internal helper methods
      std::vector<std::shared_ptr<Ledger::ChainNode>> getBlocks(uint64_t fromIndex,
                                                    uint64_t toIndex) const;

      std::vector<std::shared_ptr<Ledger::ChainNode>> chain_;
    };

    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };
    
    template <typename T> using Roe = ResultOrError<T, Error>;

    Validator();
    virtual ~Validator() = default;

    // Block operations (non-virtual, to be used by derived classes)
    Roe<const Ledger::ChainNode&> getBlock(uint64_t blockId) const;
    Roe<void> addBlockBase(const Ledger::ChainNode& block);
    Roe<void> validateBlockBase(const Ledger::ChainNode& block) const;
    uint64_t getCurrentBlockId() const;

    // Consensus queries
    uint64_t getCurrentSlot() const;
    uint64_t getCurrentEpoch() const;

    // Chain validation
    bool isChainValid(const BlockChain& chain) const;

    // Block hash calculation
    std::string calculateHash(const Ledger::Block& block) const;
    
protected:
    // Validation helpers
    bool validateBlock(const Ledger::ChainNode& block) const;
    bool isValidBlockSequence(const Ledger::ChainNode& block) const;
    bool isValidSlotLeader(const Ledger::ChainNode& block) const;
    bool isValidTimestamp(const Ledger::ChainNode& block) const;

    // Getters for derived classes
    consensus::Ouroboros& getConsensus() { return consensus_; }
    const consensus::Ouroboros& getConsensus() const { return consensus_; }
    Ledger& getLedger() { return ledger_; }
    const Ledger& getLedger() const { return ledger_; }
    BlockChain& getChainMutable() { return chain_; }
    const BlockChain& getChain() const { return chain_; }
    std::mutex& getStateMutex() const { return stateMutex_; }

    Roe<void> syncChain(const BlockChain& chain);

private:
    // Core components
    consensus::Ouroboros consensus_;
    Ledger ledger_;
    BlockChain chain_;
    
    // State tracking
    mutable std::mutex stateMutex_;
};

} // namespace pp

#endif // PP_LEDGER_VALIDATOR_H
