#ifndef PP_LEDGER_CHAIN_H
#define PP_LEDGER_CHAIN_H

#include "../client/Client.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../lib/Utilities.h"
#include "AccountBuffer.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace pp {

/**
 * Chain - Core block validation and chain management
 *
 * Provides common functionality for:
 * - Block validation
 * - Chain management
 * - Consensus integration
 * - Ledger operations
 */
class Chain : public Module {
public:
  struct CheckpointConfig {
    uint64_t minBlocks{0}; // minimum number of blocks to trigger a checkpoint
    uint64_t minAgeSeconds{
        0}; // minimum age of the blocks to trigger a checkpoint

    template <typename Archive> void serialize(Archive &ar) {
      ar &minBlocks &minAgeSeconds;
    }
  };

  // BlockChainConfig - Configuration for the block chain
  // This is used to restore the block chain from a checkpoint transaction
  struct BlockChainConfig {
    int64_t genesisTime{0};               // In seconds
    uint64_t slotDuration{0};             // In seconds
    uint64_t slotsPerEpoch{0};
    uint64_t maxCustomMetaSize{0};        // In bytes
    uint64_t maxTransactionsPerBlock{0};
    std::vector<uint16_t> minFeeCoefficients;  // a + b * sizeInNonFreeKiB + c * sizeInNonFreeKiB^2
    uint32_t freeCustomMetaSize{0};       // In bytes
    CheckpointConfig checkpoint;
    uint64_t maxValidationTimespanSeconds{0}; // Max allowed (validationTsMax - validationTsMin); must be > 0 at init

    template <typename Archive> void serialize(Archive &ar) {
      ar &genesisTime &slotDuration &slotsPerEpoch &maxCustomMetaSize
          &maxTransactionsPerBlock &minFeeCoefficients &freeCustomMetaSize &checkpoint
          &maxValidationTimespanSeconds;
    }
  };

  struct GenesisAccountMeta {
    constexpr static const uint32_t VERSION = 1;

    BlockChainConfig config;
    Client::UserAccount genesis;

    template <typename Archive> void serialize(Archive &ar) {
      ar &config &genesis;
    }

    std::string ltsToString() const;
    bool ltsFromString(const std::string &str);
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Error code groups
  // State and initialization errors (1-9)
  constexpr static int32_t E_STATE_INIT = 1;  // Ledger initialization failed
  constexpr static int32_t E_STATE_MOUNT = 2; // Ledger mount failed
  constexpr static int32_t E_INVALID_ARGUMENT = 3; // Invalid argument

  // Block validation errors (10-29)
  constexpr static int32_t E_BLOCK_NOT_FOUND = 10; // Block not found
  constexpr static int32_t E_BLOCK_SEQUENCE = 11;  // Invalid block sequence
  constexpr static int32_t E_BLOCK_HASH = 12;  // Block hash validation failed
  constexpr static int32_t E_BLOCK_INDEX = 13; // Block index mismatch
  constexpr static int32_t E_BLOCK_CHAIN = 14; // Block previous hash mismatch
  constexpr static int32_t E_BLOCK_VALIDATION =
      15; // General block validation failed
  constexpr static int32_t E_BLOCK_GENESIS =
      16; // Genesis block validation failed

  // Consensus errors (30-39)
  constexpr static int32_t E_CONSENSUS_SLOT_LEADER = 30; // Invalid slot leader
  constexpr static int32_t E_CONSENSUS_TIMING =
      31; // Block timestamp outside valid range
  constexpr static int32_t E_CONSENSUS_QUERY =
      32; // Failed to query consensus data

  // Account errors (40-59)
  constexpr static int32_t E_ACCOUNT_NOT_FOUND = 40; // Account not found
  constexpr static int32_t E_ACCOUNT_EXISTS = 41;    // Account already exists
  constexpr static int32_t E_ACCOUNT_BALANCE = 42;   // Insufficient balance
  constexpr static int32_t E_ACCOUNT_BUFFER =
      43; // Failed to add account to buffer
  constexpr static int32_t E_ACCOUNT_RENEWAL =
      44; // Account renewal validation failed

  // Transaction validation errors (60-79)
  constexpr static int32_t E_TX_VALIDATION =
      60;                                       // Transaction validation failed
  constexpr static int32_t E_TX_SIGNATURE = 61; // Invalid transaction signature
  constexpr static int32_t E_TX_FEE = 62;       // Transaction fee below minimum
  constexpr static int32_t E_TX_AMOUNT = 63;    // Invalid transaction amount
  constexpr static int32_t E_TX_TYPE = 64;      // Unknown transaction type
  constexpr static int32_t E_TX_TRANSFER = 65;  // Transaction transfer failed
  constexpr static int32_t E_TX_IDEMPOTENCY = 66;     // Duplicate idempotent id
  constexpr static int32_t E_TX_VALIDATION_TIMESPAN = 67; // Validation window exceeds max timespan
  constexpr static int32_t E_TX_TIME_OUTSIDE_WINDOW = 68; // Current time outside validation window

  // Ledger operation errors (80-89)
  constexpr static int32_t E_LEDGER_WRITE = 80; // Failed to persist to ledger
  constexpr static int32_t E_LEDGER_READ = 81;  // Failed to read from ledger

  // Internal errors (90-99)
  constexpr static int32_t E_INTERNAL_DESERIALIZE =
      90; // Deserialization failed
  constexpr static int32_t E_INTERNAL_BUFFER =
      91;                                   // Internal buffer operation failed
  constexpr static int32_t E_INTERNAL = 99; // Other internal error

  Chain();
  virtual ~Chain() = default;

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
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;
  int64_t getConsensusTimestamp() const;
  uint64_t getStakeholderStake(uint64_t stakeholderId) const;
  /** Max transactions per block (0 = no limit). Renewals are not counted toward this cap. */
  uint64_t getMaxTransactionsPerBlock() const;

  // ----------------- methods -------------------------------------
  std::string calculateHash(const Ledger::Block &block) const;
  Roe<uint64_t>
  calculateMinimumFeeFromNonFreeMetaSize(const BlockChainConfig &config,
                                         uint64_t nonFreeCustomMetaSizeBytes) const;
  Roe<size_t>
  extractNonFreeCustomMetaSizeForFee(const BlockChainConfig &config,
                                     const Ledger::Transaction &tx) const;
  Roe<uint64_t>
  calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                    const Ledger::Transaction &tx) const;
  Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>>
  collectRenewals(uint64_t slot) const;

  Roe<Ledger::ChainNode> readBlock(uint64_t blockId) const;
  Roe<Ledger::ChainNode> readLastBlock() const;

  Roe<std::vector<Ledger::SignedData<Ledger::Transaction>>>
  findTransactionsByWalletId(uint64_t walletId, uint64_t& ioBlockId) const;

  Roe<void>
  addBufferTransaction(AccountBuffer &bank,
                       const Ledger::SignedData<Ledger::Transaction> &signedTx,
                       uint64_t slotLeaderId) const;

  void initConsensus(const consensus::Ouroboros::Config &config);
  Roe<void> initLedger(const Ledger::InitConfig &config);
  Roe<void> mountLedger(const std::string &workDir);
  Roe<uint64_t> loadFromLedger(uint64_t startingBlockId);
  Roe<void> addBlock(const Ledger::ChainNode &block, bool isStrictMode);
  /** Refresh stakeholders for live mode (uses current epoch). */
  void refreshStakeholders();
  /** Refresh stakeholders for load-from-ledger (per epoch, uses block slot). */
  void refreshStakeholders(uint64_t blockSlot);

protected:
  // Validation helpers
  bool needsCheckpoint(const CheckpointConfig &checkpointConfig) const;
  uint64_t getBlockAgeSeconds(uint64_t blockId) const;

private:
  /** Maximum blocks to scan in findTransactionsByWalletId to avoid long runs. */
  constexpr static const uint64_t MAX_BLOCKS_TO_SCAN_FOR_WALLET_TX = 1000;
  constexpr static const uint64_t THRESHOLD_TXES_FOR_WALLET_TX = 1000;

  bool isValidBlockSequence(const Ledger::ChainNode &block) const;
  bool isValidSlotLeader(const Ledger::ChainNode &block) const;
  bool isValidTimestamp(const Ledger::ChainNode &block) const;

  /** Calculate the maximum blockId for account renewals at a given block. */
  Roe<uint64_t> calculateMaxBlockIdForRenewal(uint64_t atBlockId) const;

  /** Create a renewal or end-user transaction for a given account. */
  Roe<Ledger::SignedData<Ledger::Transaction>>
  createRenewalTransaction(uint64_t accountId) const;

  /** Find matching tx in block, update meta with current account state, return
   * serialized meta. Name reflects that meta is updated, not merely found. */
  Roe<GenesisAccountMeta>
  getGenesisAccountMetaFromBlock(const Ledger::Block &block) const;

  /** Get user account meta as struct (no serialize); used to avoid double
   * deserialize when caller needs to modify before serializing. */
  Roe<Client::UserAccount>
  getUserAccountMetaFromBlock(const Ledger::Block &block, uint64_t accountId) const;

  /** Account metadata for renewal: user accounts get genesis balance adjusted to
   * post-renewal (current - fee) since verifyBalance expects that. Single
   * serialize at end by using getUserAccountMetaFromBlock. */
  Roe<std::string>
  getUpdatedAccountMetadataForRenewal(const Ledger::Block &block,
                                      const AccountBuffer::Account &account,
                                      uint64_t minFee) const;

  Roe<void> validateAccountRenewals(const Ledger::ChainNode &block) const;

  /** Verify that signatures validly sign the transaction using the account's
   * public keys. */
  Roe<void>
  verifySignaturesAgainstAccount(const Ledger::Transaction &tx,
                                 const std::vector<std::string> &signatures,
                                 const AccountBuffer::Account &account) const;

  /** Ensure account exists in buffer, seeding from bank_ on demand. */
  Roe<void> ensureAccountInBuffer(AccountBuffer &bank,
                                  uint64_t accountId) const;

  Roe<void> processBlock(const Ledger::ChainNode &block, bool isStrictMode);
  Roe<void> processGenesisBlock(const Ledger::ChainNode &block);
  Roe<void> processNormalBlock(const Ledger::ChainNode &block,
                               bool isStrictMode);
  Roe<void> validateGenesisBlock(const Ledger::ChainNode &block) const;
  Roe<void> validateNormalBlock(const Ledger::ChainNode &block) const;

  Roe<void> processGenesisTxRecord(
      const Ledger::SignedData<Ledger::Transaction> &signedTx);
  Roe<void>
  processNormalTxRecord(const Ledger::SignedData<Ledger::Transaction> &signedTx,
                        uint64_t blockId, uint64_t blockSlot,
                        uint64_t slotLeaderId, bool isStrictMode);
  Roe<void>
  validateTxSignatures(const Ledger::SignedData<Ledger::Transaction> &signedTx,
                       uint64_t slotLeaderId, bool isStrictMode) const;

  /** Check that no block with slot in [slotMin, slotMax] already contains a tx with the same idempotentId and fromWalletId (idempotency is per wallet). */
  Roe<void> checkIdempotency(uint64_t idempotentId, uint64_t fromWalletId, uint64_t slotMin,
                            uint64_t slotMax) const;

  /** Validate idempotency rules (timespan, slot in window, duplicate id). effectiveSlot is current slot (submit) or block.slot (replay). */
  Roe<void> validateIdempotencyRules(const Ledger::Transaction &tx,
                                     uint64_t effectiveSlot) const;

  // System
  Roe<void> processSystemInit(const Ledger::Transaction &tx);
  Roe<GenesisAccountMeta>
  processSystemUpdateImpl(AccountBuffer &bank,
                          const Ledger::Transaction &tx,
                          uint64_t blockId) const;
  Roe<void> processSystemUpdate(const Ledger::Transaction &tx, uint64_t blockId,
                                bool isStrictMode);
  Roe<void> processBufferSystemUpdate(AccountBuffer &bank,
                                      const Ledger::Transaction &tx,
                                      uint64_t blockId) const;

  // User
  /** Shared impl: operates on bank. When isBufferMode, seeds accounts from
   * bank_ and checks existence in both. */
  Roe<void> processUserInitImpl(AccountBuffer &bank,
                                const Ledger::Transaction &tx,
                                uint64_t blockId, bool isBufferMode) const;
  Roe<void> processUserInit(const Ledger::Transaction &tx, uint64_t blockId);
  Roe<void> processBufferUserInit(AccountBuffer &bank,
                                  const Ledger::Transaction &tx, uint64_t blockId) const;

  Roe<void> processUserAccountUpsertImpl(
      AccountBuffer &bank, const Ledger::Transaction &tx, uint64_t blockId,
      bool isBufferMode, bool isStrictMode) const;

  Roe<void> processUserAccountUpsert(const Ledger::Transaction &tx,
                                     uint64_t blockId, bool isStrictMode);
  Roe<void> processUserUpdate(const Ledger::Transaction &tx, uint64_t blockId,
                              bool isStrictMode);
  Roe<void> processUserRenewal(const Ledger::Transaction &tx, uint64_t blockId,
                               bool isStrictMode);
  Roe<void> processGenesisRenewal(const Ledger::Transaction &tx, uint64_t blockId,
                                  bool isStrictMode);
  Roe<void> processGenesisRenewalImpl(AccountBuffer &bank,
                                     const Ledger::Transaction &tx,
                                     uint64_t blockId,
                                     bool isStrictMode) const;
  Roe<void> processBufferUserAccountUpsert(AccountBuffer &bank,
                                           const Ledger::Transaction &tx,
                                           uint64_t blockId) const;
  Roe<void> processBufferGenesisRenewal(AccountBuffer &bank,
                                        const Ledger::Transaction &tx,
                                        uint64_t blockId) const;

  Roe<uint64_t>
  calculateMinimumFeeForAccountMeta(const AccountBuffer &bank,
                                    uint64_t accountId) const;

  Roe<void> processUserEndImpl(AccountBuffer &bank,
                              const Ledger::Transaction &tx,
                              bool isBufferMode) const;
  Roe<void> processUserEnd(const Ledger::Transaction &tx, uint64_t blockId,
                           bool isStrictMode);
  Roe<void> processBufferUserEnd(AccountBuffer &bank,
                                 const Ledger::Transaction &tx) const;

  Roe<void> processBufferTransaction(AccountBuffer &bank,
                                     const Ledger::Transaction &signedTx) const;
  Roe<void> processTransaction(const Ledger::Transaction &tx, uint64_t blockId,
                               bool isStrictMode);
  Roe<void> strictProcessTransaction(AccountBuffer &bank,
                                     const Ledger::Transaction &tx) const;
  Roe<void> looseProcessTransaction(const Ledger::Transaction &tx);

  consensus::Ouroboros consensus_;
  Ledger ledger_;
  AccountBuffer bank_;
  BlockChainConfig chainConfig_;
  uint64_t currentCheckpointId_{0};
  uint64_t lastCheckpointId_{0};
};

std::ostream &operator<<(std::ostream &os,
                         const Chain::CheckpointConfig &config);
std::ostream &operator<<(std::ostream &os,
                         const Chain::BlockChainConfig &config);

} // namespace pp

#endif // PP_LEDGER_CHAIN_H
