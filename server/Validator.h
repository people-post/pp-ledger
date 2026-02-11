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

    // Error code groups
    // State and initialization errors (1-9)
    constexpr static int32_t E_STATE_INIT = 1;              // Ledger initialization failed
    constexpr static int32_t E_STATE_MOUNT = 2;             // Ledger mount failed
    
    // Block validation errors (10-29)
    constexpr static int32_t E_BLOCK_NOT_FOUND = 10;        // Block not found
    constexpr static int32_t E_BLOCK_SEQUENCE = 11;         // Invalid block sequence
    constexpr static int32_t E_BLOCK_HASH = 12;             // Block hash validation failed
    constexpr static int32_t E_BLOCK_INDEX = 13;            // Block index mismatch
    constexpr static int32_t E_BLOCK_CHAIN = 14;            // Block previous hash mismatch
    constexpr static int32_t E_BLOCK_VALIDATION = 15;       // General block validation failed
    constexpr static int32_t E_BLOCK_GENESIS = 16;          // Genesis block validation failed
    
    // Consensus errors (30-39)
    constexpr static int32_t E_CONSENSUS_SLOT_LEADER = 30;  // Invalid slot leader
    constexpr static int32_t E_CONSENSUS_TIMING = 31;       // Block timestamp outside valid range
    constexpr static int32_t E_CONSENSUS_QUERY = 32;        // Failed to query consensus data
    
    // Account errors (40-59)
    constexpr static int32_t E_ACCOUNT_NOT_FOUND = 40;      // Account not found
    constexpr static int32_t E_ACCOUNT_EXISTS = 41;         // Account already exists
    constexpr static int32_t E_ACCOUNT_BALANCE = 42;        // Insufficient balance
    constexpr static int32_t E_ACCOUNT_BUFFER = 43;         // Failed to add account to buffer
    constexpr static int32_t E_ACCOUNT_RENEWAL = 44;        // Account renewal validation failed
    
    // Transaction validation errors (60-79)
    constexpr static int32_t E_TX_VALIDATION = 60;          // Transaction validation failed
    constexpr static int32_t E_TX_SIGNATURE = 61;           // Invalid transaction signature
    constexpr static int32_t E_TX_FEE = 62;                 // Transaction fee below minimum
    constexpr static int32_t E_TX_AMOUNT = 63;              // Invalid transaction amount
    constexpr static int32_t E_TX_TYPE = 64;                // Unknown transaction type
    constexpr static int32_t E_TX_TRANSFER = 65;            // Transaction transfer failed
    
    // Ledger operation errors (80-89)
    constexpr static int32_t E_LEDGER_WRITE = 80;           // Failed to persist to ledger
    constexpr static int32_t E_LEDGER_READ = 81;            // Failed to read from ledger
    
    // Internal errors (90-99)
    constexpr static int32_t E_INTERNAL_DESERIALIZE = 90;   // Deserialization failed
    constexpr static int32_t E_INTERNAL_BUFFER = 91;        // Internal buffer operation failed
    constexpr static int32_t E_INTERNAL = 99;               // Other internal error

    Validator();
    virtual ~Validator() = default;

    // ----------------- accessors -------------------------------------
    bool isStakeholderSlotLeader(uint64_t stakeholderId, uint64_t slot) const;
    bool isSlotBlockProductionTime(uint64_t slot) const;

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
    int64_t getConsensusTimestamp() const;
    uint64_t getStakeholderStake(uint64_t stakeholderId) const;

    // ----------------- methods -------------------------------------
    std::string calculateHash(const Ledger::Block& block) const;
    Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>> collectRenewals(uint64_t slot) const;
    Roe<Ledger::ChainNode> readLastBlock() const;
    Roe<void> addBufferTransaction(AccountBuffer& bank, const Ledger::SignedData<Ledger::Transaction>& signedTx) const;

    void initConsensus(const consensus::Ouroboros::Config& config);
    Roe<void> initLedger(const Ledger::InitConfig& config);
    Roe<void> mountLedger(const std::string& workDir);
    Roe<uint64_t> loadFromLedger(uint64_t startingBlockId);
    Roe<void> addBlock(const Ledger::ChainNode& block, bool isStrictMode);
    void refreshStakeholders();
    
protected:
    // Validation helpers
    bool needsCheckpoint(const CheckpointConfig& checkpointConfig) const;
    uint64_t getBlockAgeSeconds(uint64_t blockId) const;

private:
    bool isValidBlockSequence(const Ledger::ChainNode& block) const;
    bool isValidSlotLeader(const Ledger::ChainNode& block) const;
    bool isValidTimestamp(const Ledger::ChainNode& block) const;
    Roe<void> validateAccountRenewals(const Ledger::ChainNode& block) const;

    Roe<void> processBlock(const Ledger::ChainNode& block, bool isStrictMode);
    Roe<void> validateBlock(const Ledger::ChainNode& block) const;
    Roe<void> validateGenesisBlock(const Ledger::ChainNode& block) const;
    Roe<void> validateNormalBlock(const Ledger::ChainNode& block) const;

    Roe<void> processTxRecord(const Ledger::SignedData<Ledger::Transaction>& signedTx, uint64_t blockId, bool isStrictMode);
    Roe<void> validateTxSignatures(const Ledger::SignedData<Ledger::Transaction>& signedTx, bool isStrictMode);

    Roe<void> processSystemInit(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> processSystemUpdate(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> processUserInit(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> processUserUpdate(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);

    Roe<void> processUserRenewal(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> processUserEnd(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);

    Roe<void> processBufferTransaction(AccountBuffer& bank, const Ledger::Transaction& signedTx) const;
    Roe<void> processTransaction(const Ledger::Transaction& tx, uint64_t blockId, bool isStrictMode);
    Roe<void> strictProcessTransaction(AccountBuffer& bank, const Ledger::Transaction& tx) const;
    Roe<void> looseProcessTransaction(const Ledger::Transaction& tx);

    /** Create a renewal or end-user transaction for a given account. */
    Roe<Ledger::SignedData<Ledger::Transaction>> createRenewalTransaction(uint64_t accountId, uint64_t minFee) const;

    /** Calculate the maximum blockId for account renewals at a given block. */
    Roe<uint64_t> calculateMaxBlockIdForRenewal(uint64_t atBlockId) const;

    /** Find and update account metadata from a block's transactions. */
    Roe<std::string> findAccountMetadataInBlock(const Ledger::Block& block, const AccountBuffer::Account& account) const;

    /** Build serialized UserAccount meta from the account currently in the buffer. */
    Roe<std::string> updateMetaFromSystemInit(const std::string& meta) const;
    Roe<std::string> updateMetaFromSystemUpdate(const std::string& meta) const;
    Roe<std::string> updateMetaFromUserInit(const std::string& meta, const AccountBuffer::Account& account) const;
    Roe<std::string> updateMetaFromUserUpdate(const std::string& meta, const AccountBuffer::Account& account) const;
    Roe<std::string> updateMetaFromUserRenewal(const std::string& meta, const AccountBuffer::Account& account) const;
    Roe<std::string> updateMetaFromUserEnd(const std::string& meta, const AccountBuffer::Account& account) const;

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
