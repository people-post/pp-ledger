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
 * Validator - Core block validation and chain management
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
      uint64_t minBlocks{ 0 }; // minimum number of blocks to trigger a checkpoint
      uint64_t minAgeSeconds{ 0 }; // minimum age of the blocks to trigger a checkpoint

      template <typename Archive> void serialize(Archive &ar) {
        ar & minBlocks & minAgeSeconds;
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
        ar & genesisTime & slotDuration & slotsPerEpoch & maxPendingTransactions
         & maxTransactionsPerBlock & minFeePerTransaction & checkpoint;
      }
    };

    struct SystemCheckpoint {
      constexpr static const uint32_t VERSION = 1;

      BlockChainConfig config;
      Client::UserAccount genesis;

      template <typename Archive> void serialize(Archive &ar) {
        ar & config & genesis;
      }

      std::string ltsToString() const;
      bool ltsFromString(const std::string& str);
    };

    struct Error : RoeErrorBase {
        using RoeErrorBase::RoeErrorBase;
    };
    
    template <typename T> using Roe = ResultOrError<T, Error>;

    constexpr static int32_t E_INPUT = 1; // Invalid input data
    constexpr static int32_t E_STATE = 2; // Invalid state for the requested operation
    constexpr static int32_t E_LEDGER = 3; // Ledger operation failed
    constexpr static int32_t E_CONSENSUS = 4; // Consensus validation failed
    constexpr static int32_t E_VALIDATION = 5; // Block or transaction validation failed
    constexpr static int32_t E_INTERNAL = 6; // Internal error (e.g. serialization failure)
    constexpr static int32_t E_UNAUTHORIZED = 7; // Unauthorized action

    Validator();
    virtual ~Validator() = default;

    // ----------------- accessors -------------------------------------
    uint64_t getNextBlockId() const;
    uint64_t getLastCheckpointId() const;
    uint64_t getCurrentCheckpointId() const;
    uint64_t getCurrentSlot() const;
    uint64_t getCurrentEpoch() const;
    uint64_t getTotalStake() const;
    Roe<uint64_t> getSlotLeader(uint64_t slot) const;
    std::vector<consensus::Stakeholder> getStakeholders() const;
    Roe<Ledger::ChainNode> getBlock(uint64_t blockId) const;
    Roe<Client::UserAccount> getAccount(uint64_t accountId) const;

    // ----------------- methods -------------------------------------
    std::string calculateHash(const Ledger::Block& block) const;

    // ----------------- core operations -------------------------------------
    bool isStakeholderSlotLeader(uint64_t stakeholderId, uint64_t slot) const;
    bool isSlotBlockProductionTime(uint64_t slot) const;
    int64_t getConsensusTimestamp() const;
    uint64_t getStakeholderStake(uint64_t stakeholderId) const;
    Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>> collectRenewals(uint64_t slot) const;
    Roe<Ledger::ChainNode> readLastBlock() const;

    void initConsensus(const consensus::Ouroboros::Config& config);
    Roe<void> initLedger(const Ledger::InitConfig& config);
    Roe<void> mountLedger(const std::string& workDir);
    Roe<void> addBlock(const Ledger::ChainNode& block, bool isStrictMode);
    Roe<void> addBufferTransaction(AccountBuffer& bufferBank, const Ledger::SignedData<Ledger::Transaction>& signedTx) const;
    void refreshStakeholders();
    Roe<uint64_t> loadFromLedger(uint64_t startingBlockId);
    
protected:
    // Validation helpers
    bool needsCheckpoint(const CheckpointConfig& checkpointConfig) const;
    uint64_t getBlockAgeSeconds(uint64_t blockId) const;

private:
    bool isValidBlockSequence(const Ledger::ChainNode& block) const;
    bool isValidSlotLeader(const Ledger::ChainNode& block) const;
    bool isValidTimestamp(const Ledger::ChainNode& block) const;

    Roe<void> processBlock(const Ledger::ChainNode& block, bool isStrictMode);
    Roe<void> validateBlock(const Ledger::ChainNode& block) const;
    Roe<void> validateGenesisBlock(const Ledger::ChainNode& block) const;
    Roe<void> validateNormalBlock(const Ledger::ChainNode& block) const;

    Roe<void> processTxRecord(const Ledger::SignedData<Ledger::Transaction>& signedTx, uint64_t blockId, bool isStrictMode);
    Roe<void> validateTxSignatures(const Ledger::SignedData<Ledger::Transaction>& signedTx, bool isStrictMode);
    Roe<void> processSystemCheckpoint(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> processNewUser(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> processUserCheckpoint(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> processTransaction(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> strictProcessTransaction(const Ledger::Transaction& tx);
    Roe<void> looseProcessTransaction(const Ledger::Transaction& tx);

    /** Create a renewal or end-user transaction for a given account. */
    Roe<Ledger::SignedData<Ledger::Transaction>> createRenewalTransaction(uint64_t accountId, uint64_t minFee) const;

    /** Find and update account metadata from a block's transactions. */
    Roe<std::string> findAccountMetadataInBlock(const Ledger::Block& block, const AccountBuffer::Account& account) const;

    /** Build serialized UserAccount meta from the account currently in the buffer. */
    Roe<std::string> updateMetaFromCheckpoint(const std::string& meta) const;
    Roe<std::string> updateMetaFromNewUser(const std::string& meta, const AccountBuffer::Account& account) const;
    Roe<std::string> updateMetaFromUser(const std::string& meta, const AccountBuffer::Account& account) const;
    Roe<std::string> updateMetaFromRenewal(const std::string& meta, const AccountBuffer::Account& account) const;
    Roe<std::string> updateMetaFromEndUser(const std::string& meta, const AccountBuffer::Account& account) const;

    consensus::Ouroboros consensus_;
    Ledger ledger_;
    AccountBuffer bank_;
    BlockChainConfig chainConfig_;
    uint64_t currentCheckpointId_{ 0 };
    uint64_t lastCheckpointId_{ 0 };
};

std::ostream& operator<<(std::ostream& os, const Validator::CheckpointConfig& config);
std::ostream& operator<<(std::ostream& os, const Validator::BlockChainConfig& config);

} // namespace pp

#endif // PP_LEDGER_VALIDATOR_H
