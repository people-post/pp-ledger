#ifndef PP_LEDGER_CHAIN_H
#define PP_LEDGER_CHAIN_H

#include "../client/Client.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "lib/common/Module.h"
#include "lib/common/ResultOrError.hpp"
#include "lib/common/Crypto.h"
#include "lib/common/Utilities.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxContext.h"
#include "TxError.h"
#include "Types.h"
#include "ITxHandler.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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
  using Checkpoint = ::pp::Checkpoint;
  using CheckpointConfig = ::pp::CheckpointConfig;
  using BlockChainConfig = ::pp::BlockChainConfig;
  using GenesisAccountMeta = ::pp::GenesisAccountMeta;

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
    Error(const chain_tx::TxError &e) : RoeErrorBase(e.code, e.message) {}
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Error code groups
  constexpr static int32_t E_STATE_INIT = chain_err::E_STATE_INIT;
  constexpr static int32_t E_STATE_MOUNT = chain_err::E_STATE_MOUNT;
  constexpr static int32_t E_INVALID_ARGUMENT = chain_err::E_INVALID_ARGUMENT;

  constexpr static int32_t E_BLOCK_NOT_FOUND = chain_err::E_BLOCK_NOT_FOUND;
  constexpr static int32_t E_BLOCK_SEQUENCE = chain_err::E_BLOCK_SEQUENCE;
  constexpr static int32_t E_BLOCK_HASH = chain_err::E_BLOCK_HASH;
  constexpr static int32_t E_BLOCK_INDEX = chain_err::E_BLOCK_INDEX;
  constexpr static int32_t E_BLOCK_CHAIN = chain_err::E_BLOCK_CHAIN;
  constexpr static int32_t E_BLOCK_VALIDATION = chain_err::E_BLOCK_VALIDATION;
  constexpr static int32_t E_BLOCK_GENESIS = chain_err::E_BLOCK_GENESIS;

  constexpr static int32_t E_CONSENSUS_SLOT_LEADER =
      chain_err::E_CONSENSUS_SLOT_LEADER;
  constexpr static int32_t E_CONSENSUS_TIMING = chain_err::E_CONSENSUS_TIMING;
  constexpr static int32_t E_CONSENSUS_QUERY = chain_err::E_CONSENSUS_QUERY;

  constexpr static int32_t E_ACCOUNT_NOT_FOUND = chain_err::E_ACCOUNT_NOT_FOUND;
  constexpr static int32_t E_ACCOUNT_EXISTS = chain_err::E_ACCOUNT_EXISTS;
  constexpr static int32_t E_ACCOUNT_BALANCE = chain_err::E_ACCOUNT_BALANCE;
  constexpr static int32_t E_ACCOUNT_BUFFER = chain_err::E_ACCOUNT_BUFFER;
  constexpr static int32_t E_ACCOUNT_RENEWAL = chain_err::E_ACCOUNT_RENEWAL;

  constexpr static int32_t E_TX_VALIDATION = chain_err::E_TX_VALIDATION;
  constexpr static int32_t E_TX_SIGNATURE = chain_err::E_TX_SIGNATURE;
  constexpr static int32_t E_TX_FEE = chain_err::E_TX_FEE;
  constexpr static int32_t E_TX_AMOUNT = chain_err::E_TX_AMOUNT;
  constexpr static int32_t E_TX_TYPE = chain_err::E_TX_TYPE;
  constexpr static int32_t E_TX_TRANSFER = chain_err::E_TX_TRANSFER;
  constexpr static int32_t E_TX_IDEMPOTENCY = chain_err::E_TX_IDEMPOTENCY;
  constexpr static int32_t E_TX_VALIDATION_TIMESPAN =
      chain_err::E_TX_VALIDATION_TIMESPAN;
  constexpr static int32_t E_TX_TIME_OUTSIDE_WINDOW =
      chain_err::E_TX_TIME_OUTSIDE_WINDOW;

  constexpr static int32_t E_LEDGER_WRITE = chain_err::E_LEDGER_WRITE;
  constexpr static int32_t E_LEDGER_READ = chain_err::E_LEDGER_READ;

  constexpr static int32_t E_INTERNAL_DESERIALIZE =
      chain_err::E_INTERNAL_DESERIALIZE;
  constexpr static int32_t E_INTERNAL_BUFFER = chain_err::E_INTERNAL_BUFFER;
  constexpr static int32_t E_INTERNAL = chain_err::E_INTERNAL;

  Chain();
  ~Chain() override = default;

  // ----------------- accessors -------------------------------------
  bool isStakeholderSlotLeader(uint64_t stakeholderId, uint64_t slot) const;
  bool isSlotBlockProductionTime(uint64_t slot) const;
  /** True when chain config has been loaded (from T_GENESIS or T_CONFIG). */
  bool isChainConfigReady() const;

  uint64_t getNextBlockId() const;
  Checkpoint getCheckpoint() const;
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  uint64_t getTotalStake() const;
  Roe<uint64_t> getSlotLeader(uint64_t slot) const;
  std::vector<consensus::Stakeholder> getStakeholders() const;
  Roe<Client::UserAccount> getAccount(uint64_t accountId) const;
  int64_t getConsensusTimestamp() const;
  /** Start time of the given slot (consensus timestamp). */
  int64_t getSlotStartTime(uint64_t slot) const;
  /** Slot duration in seconds. */
  uint64_t getSlotDuration() const;
  uint64_t getStakeholderStake(uint64_t stakeholderId) const;
  /** Max transactions per block (0 = no limit). Renewals are not counted toward
   * this cap. */
  uint64_t getMaxTransactionsPerBlock() const;

  // ----------------- methods -------------------------------------
  std::string calculateHash(const Ledger::Block &block) const;
  Roe<uint64_t> calculateMinimumFeeFromNonFreeMetaSize(
      const BlockChainConfig &config,
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
  findTransactionsByWalletId(uint64_t walletId, uint64_t &ioBlockId) const;
  Roe<Ledger::SignedData<Ledger::Transaction>>
  findTransactionByIndex(uint64_t txIndex) const;

  Roe<void>
  addBufferTransaction(AccountBuffer &bank,
                       const Ledger::SignedData<Ledger::Transaction> &signedTx,
                       uint64_t slotLeaderId) const;

  void initConsensus(const consensus::Ouroboros::Config &config);
  Roe<void> initLedger(const Ledger::InitConfig &config);
  Roe<void> mountLedger(const std::string &workDir);
  Roe<uint64_t> loadFromLedger(uint64_t startingBlockId);
  Roe<void> addBlock(const Ledger::ChainNode &block);
  /** Refresh stakeholders for live mode (uses current epoch). */
  void refreshStakeholders();
  /** Refresh stakeholders for load-from-ledger (per epoch, uses block slot). */
  void refreshStakeholders(uint64_t blockSlot);

  /** Non-owning view of chain subsystems for transaction handlers (Phase 2). */
  ChainTxContext transactionContext();
  /** Const overload: const refs only (logger stays non-const for output). */
  ChainTxContextConst transactionContext() const;

protected:
  // Validation helpers
  bool needsCheckpoint(const BlockChainConfig &config) const;
  uint64_t getBlockAgeSeconds(uint64_t blockId) const;

private:
  /** Maximum blocks to scan in findTransactionsByWalletId to avoid long runs.
   */
  constexpr static const uint64_t MAX_BLOCKS_TO_SCAN_FOR_WALLET_TX = 32;
  constexpr static const uint64_t THRESHOLD_TXES_FOR_WALLET_TX = 32;

  bool isValidSlotLeader(const Ledger::ChainNode &block) const;
  bool isValidTimestamp(const Ledger::ChainNode &block) const;
  bool shouldUseStrictMode(uint64_t blockIndex) const;

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
  getUserAccountMetaFromBlock(const Ledger::Block &block,
                              uint64_t accountId) const;

  /** Account metadata for renewal: user accounts get genesis balance adjusted
   * to post-renewal (current - fee) since verifyBalance expects that. Single
   * serialize at end by using getUserAccountMetaFromBlock. */
  Roe<std::string>
  getUpdatedAccountMetadataForRenewal(const Ledger::Block &block,
                                      const AccountBuffer::Account &account,
                                      uint64_t minFee) const;

  Roe<void> validateBlockSequence(const Ledger::ChainNode &block) const;
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
  Roe<void> validateNormalBlock(const Ledger::ChainNode &block, bool isStrictMode) const;

  /** Validate that within a single block there is at most one transaction per
   * (fromWalletId, idempotentId) pair for idempotent-aware transaction types. */
  Roe<void> validateIntraBlockIdempotency(const Ledger::ChainNode &block) const;

  Roe<void> processGenesisTxRecord(
      const Ledger::SignedData<Ledger::Transaction> &signedTx);
  Roe<void>
  processNormalTxRecord(const Ledger::SignedData<Ledger::Transaction> &signedTx,
                        uint64_t blockId, uint64_t blockSlot,
                        uint64_t slotLeaderId, bool isStrictMode);
  Roe<void>
  validateTxSignatures(const Ledger::SignedData<Ledger::Transaction> &signedTx,
                       uint64_t slotLeaderId, bool isStrictMode) const;

  /** Check that no block with slot in [slotMin, slotMax] already contains a tx
   * with the same idempotentId and fromWalletId (idempotency is per wallet). */
  Roe<void> checkIdempotency(uint64_t idempotentId, uint64_t fromWalletId,
                             uint64_t slotMin, uint64_t slotMax) const;

  /** Validate idempotency rules (timespan, slot in window, duplicate id).
   * effectiveSlot is current slot (submit) or block.slot (replay). */
  Roe<void> validateIdempotencyRules(const Ledger::Transaction &tx,
                                     uint64_t effectiveSlot, bool isStrictMode) const;

  // System
  Roe<void> processSystemUpdate(const Ledger::Transaction &tx, uint64_t blockId,
                                bool isStrictMode);
  Roe<void> processBufferSystemUpdate(AccountBuffer &bank,
                                      const Ledger::Transaction &tx,
                                      uint64_t blockId) const;

  // User
  Roe<void> processUserInit(const Ledger::Transaction &tx, uint64_t blockId, bool isStrictMode);
  Roe<void> processBufferUserInit(AccountBuffer &bank,
                                  const Ledger::Transaction &tx,
                                  uint64_t blockId) const;

  Roe<void> processUserAccountUpsertImpl(AccountBuffer &bank,
                                         const Ledger::Transaction &tx,
                                         uint64_t blockId, bool isBufferMode,
                                         bool isStrictMode) const;

  Roe<void> processUserAccountUpsert(const Ledger::Transaction &tx,
                                     uint64_t blockId, bool isStrictMode);
  Roe<void> processUserUpdate(const Ledger::Transaction &tx, uint64_t blockId,
                              bool isStrictMode);
  Roe<void> processUserRenewal(const Ledger::Transaction &tx, uint64_t blockId,
                               bool isStrictMode);
  Roe<void> processGenesisRenewal(const Ledger::Transaction &tx,
                                  uint64_t blockId, bool isStrictMode);
  Roe<void> processGenesisRenewalImpl(AccountBuffer &bank,
                                      const Ledger::Transaction &tx,
                                      uint64_t blockId, bool isBufferMode,
                                      bool isStrictMode) const;
  Roe<void> processBufferUserAccountUpsert(AccountBuffer &bank,
                                           const Ledger::Transaction &tx,
                                           uint64_t blockId) const;
  Roe<void> processBufferGenesisRenewal(AccountBuffer &bank,
                                        const Ledger::Transaction &tx,
                                        uint64_t blockId) const;

  Roe<uint64_t> calculateMinimumFeeForAccountMeta(const AccountBuffer &bank,
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

  Crypto crypto_;
  consensus::Ouroboros consensus_;
  Ledger ledger_;
  AccountBuffer bank_;
  std::optional<BlockChainConfig> optChainConfig_{std::nullopt};
  Checkpoint checkpoint_{};

  /** One slot per Ledger::Transaction type (0..6); nullptr = not migrated yet. */
  std::array<std::unique_ptr<ITxHandler>, 7> transactionHandlers_{};
};

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config);
std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config);

} // namespace pp

#endif // PP_LEDGER_CHAIN_H
