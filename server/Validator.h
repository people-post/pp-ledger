#ifndef PP_LEDGER_VALIDATOR_H
#define PP_LEDGER_VALIDATOR_H

#include "AccountBuffer.h"
#include "../ledger/Ledger.h"
#include "../client/Client.h"
#include "../consensus/Ouroboros.h"
#include "../lib/Module.h"
#include "../lib/Utilities.h"
#include "../lib/ResultOrError.hpp"

#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include <ostream>

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
    struct CheckpointConfig {
      uint64_t minSizeBytes{ 0 };
      uint64_t ageSeconds{ 0 };

      template <typename Archive> void serialize(Archive &ar) {
        ar & minSizeBytes & ageSeconds;
      }
    };

    // BlockChainConfig - Configuration for the block chain
    // This is used to restore the block chain from a checkpoint transaction
    struct BlockChainConfig {
      int64_t genesisTime{ 0 };
      uint64_t slotDuration{ 0 };
      uint64_t slotsPerEpoch{ 0 };
      uint64_t maxPendingTransactions{ 0 };
      uint64_t maxTransactionsPerBlock{ 0 };
      uint64_t minFeePerTransaction{ 0 };
      CheckpointConfig checkpoint;

      template <typename Archive> void serialize(Archive &ar) {
        ar & genesisTime & slotDuration & slotsPerEpoch & maxPendingTransactions & maxTransactionsPerBlock & minFeePerTransaction & checkpoint;
      }
    };

    struct SingleTokenAccountInfo {
      constexpr static const uint32_t VERSION = 1;

      int64_t balance{ 0 };
      std::vector<std::string> publicKeys;
      std::string meta;

      template <typename Archive> void serialize(Archive &ar) {
        ar & balance & publicKeys & meta;
      }

      std::string ltsToString() const;
      bool ltsFromString(const std::string& str);
    };

    struct SystemCheckpoint {
      constexpr static const uint32_t VERSION = 1;

      BlockChainConfig config;
      SingleTokenAccountInfo genesis;
      SingleTokenAccountInfo fee;
      SingleTokenAccountInfo reserve;

      template <typename Archive> void serialize(Archive &ar) {
        ar & config & genesis & fee & reserve;
      }

      std::string ltsToString() const;
      bool ltsFromString(const std::string& str);
    };

    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };
    
    template <typename T> using Roe = ResultOrError<T, Error>;

    Validator();
    virtual ~Validator() = default;

    // ----------------- accessors -------------------------------------
    bool isChainValid(const std::vector<Ledger::ChainNode>& chain) const;

    uint64_t getNextBlockId() const;
    uint64_t getCurrentSlot() const;
    uint64_t getCurrentEpoch() const;
    uint64_t getTotalStake() const;
    std::vector<consensus::Stakeholder> getStakeholders() const;
    Roe<Ledger::ChainNode> getBlock(uint64_t blockId) const;
    Roe<Client::AccountInfo> getAccount(uint64_t accountId) const;

    // ----------------- methods -------------------------------------
    std::string calculateHash(const Ledger::Block& block) const;
    
protected:
    // Validation helpers
    bool isValidBlockSequence(const Ledger::ChainNode& block) const;
    bool isValidSlotLeader(const Ledger::ChainNode& block) const;
    bool isValidTimestamp(const Ledger::ChainNode& block) const;

    Roe<void> validateBlock(const Ledger::ChainNode& block) const;
    Roe<void> validateGenesisBlock(const Ledger::ChainNode& block) const;

    // Getters for derived classes
    consensus::Ouroboros& getConsensus() { return consensus_; }
    const consensus::Ouroboros& getConsensus() const { return consensus_; }
    Ledger& getLedger() { return ledger_; }
    const Ledger& getLedger() const { return ledger_; }

    Roe<void> addBlockBase(const Ledger::ChainNode& block, bool isStrictMode);
    Roe<void> addBufferTransaction(AccountBuffer& bufferBank, const Ledger::Transaction& tx);
    void refreshStakeholders();

    Roe<uint64_t> loadFromLedger(uint64_t startingBlockId);
    Roe<void> processBlock(const Ledger::ChainNode& block, uint64_t blockId, bool isStrictMode);
    Roe<void> processTransaction(const Ledger::Transaction& tx, bool isStrictMode);
    Roe<void> processSystemCheckpoint(const Ledger::Transaction& tx);
    Roe<void> processUserCheckpoint(const Ledger::Transaction& tx);
    Roe<void> processTransaction(const Ledger::Transaction& tx);
    Roe<void> looseProcessTransaction(const Ledger::Transaction& tx);

private:
    // Core components
    consensus::Ouroboros consensus_;
    Ledger ledger_;
    AccountBuffer bank_;
    BlockChainConfig chainConfig_;
};

inline std::ostream& operator<<(std::ostream& os, const Validator::SingleTokenAccountInfo& info) {
  os << "SingleTokenAccountInfo{balance: " << info.balance << ", publicKeys: [" << utl::join(info.publicKeys, ", ") << "], meta: \"" << info.meta << "\"}";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const Validator::BlockChainConfig& config) {
  os << "BlockChainConfig{genesisTime=" << config.genesisTime << ", "
     << "slotDuration=" << config.slotDuration << ", "
     << "slotsPerEpoch=" << config.slotsPerEpoch << ", "
     << "maxPendingTransactions=" << config.maxPendingTransactions << ", "
     << "maxTransactionsPerBlock=" << config.maxTransactionsPerBlock << ", "
     << "minFeePerTransaction=" << config.minFeePerTransaction << "}";
  return os;
}

} // namespace pp

#endif // PP_LEDGER_VALIDATOR_H
