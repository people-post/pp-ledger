#ifndef PP_LEDGER_VALIDATOR_H
#define PP_LEDGER_VALIDATOR_H

#include "../ledger/Ledger.h"
#include "../ledger/Block.h"
#include "../ledger/BlockChain.h"
#include "../consensus/Ouroboros.h"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"

#include <string>
#include <cstdint>
#include <memory>
#include <mutex>

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
    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };
    
    template <typename T> using Roe = ResultOrError<T, Error>;

    struct BaseConfig {
        std::string workDir;
        uint64_t slotDuration = 1; // seconds
        uint64_t slotsPerEpoch = 21600; // ~6 hours
    };

    Validator();
    virtual ~Validator() = default;

    // Block operations (non-virtual, to be used by derived classes)
    Roe<std::shared_ptr<Block>> getBlockBase(uint64_t blockId) const;
    Roe<void> addBlockBase(const Block& block);
    Roe<void> validateBlockBase(const Block& block) const;
    uint64_t getCurrentBlockId() const;

    // Consensus queries
    uint64_t getCurrentSlot() const;
    uint64_t getCurrentEpoch() const;
    
    // Chain access
    const BlockChain& getChain() const { return chain_; }

protected:
    // Initialization helper
    Roe<void> initBase(const BaseConfig& config);
    
    // Validation helpers
    bool isValidBlockSequence(const Block& block) const;
    bool isValidSlotLeader(const Block& block) const;
    bool isValidTimestamp(const Block& block) const;

    // Core components (protected so derived classes can access)
    consensus::Ouroboros consensus_;
    Ledger ledger_;
    BlockChain chain_;
    
    // Configuration
    BaseConfig baseConfig_;
    
    // State tracking
    mutable std::mutex stateMutex_;
};

} // namespace pp

#endif // PP_LEDGER_VALIDATOR_H
