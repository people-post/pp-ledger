#pragma once

#include "ITxHandler.h"
#include "TxError.h"
#include "Types.h"
#include "../ledger/Ledger.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace pp {

/** RecordHandler owns the per-record-type tx handlers. */
class RecordHandler final {
public:
  static constexpr std::size_t kNumTxTypes = 7;

  RecordHandler();
  ~RecordHandler() = default;

  RecordHandler(const RecordHandler &) = delete;
  RecordHandler &operator=(const RecordHandler &) = delete;
  RecordHandler(RecordHandler &&) = delete;
  RecordHandler &operator=(RecordHandler &&) = delete;

  /** Get handler for a ledger record type id (0..6). */
  ITxHandler *get(std::size_t type);
  const ITxHandler *get(std::size_t type) const;

  /**
   * Decode `rec`, dispatch to its tx handler, and return whether the tx is
   * indexed for `walletId`. Decode failure, unknown type, handler errors, or
   * a negative match all yield false.
   */
  bool matchesWalletForIndex(const Ledger::Record &rec,
                             uint64_t walletId) const;

  /**
   * Decode `rec`, dispatch to its handler, and return getSignerAccountId.
   * Decode / missing-handler use the same codes as applyBuffer. Handler
   * failures are reported as E_TX_SIGNATURE with the same message prefix as
   * Chain::validateTxSignatures.
   */
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::Record &rec, uint64_t slotLeaderId) const;

  /**
   * Serialized user-account metadata blob from a single record, if this record
   * updates the given non-genesis account (new user / user update / renewal).
   */
  std::optional<std::string>
  getUserAccountMeta(const Ledger::Record &rec, uint64_t accountId) const;

  /**
   * Serialized genesis checkpoint metadata from a single record, if applicable
   * (genesis on block 0, config meta, or genesis renewal).
   */
  std::optional<std::string>
  getGenesisAccountMeta(const Ledger::Record &rec,
                        const Ledger::Block &block) const;

  /**
   * If this record participates in idempotency rules, return (walletId,
   * idempotentId). Decode failure or non-participating types yield nullopt
   * without error (scan skips the record).
   */
  chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
  getIdempotencyKey(const Ledger::Record &rec) const;

  /**
   * Billable (pre-free-tier) custom-meta size for fee calculation.
   *
   * This is tx-type aware (e.g. serialized user account meta in tx.meta).
   */
  chain_tx::Roe<size_t>
  getBillableCustomMetaSizeForFee(const BlockChainConfig &config,
                                  const Ledger::TypedTx &tx) const;

  /**
   * Decode `rec`, dispatch to its handler, and run applyBuffer. Mirrors
   * Chain's decode / handler-not-registered errors.
   */
  chain_tx::Roe<void> applyBuffer(const Ledger::Record &rec, AccountBuffer &bank,
                                  const BufferApplyContext &ctx) const;

  /**
   * Decode `rec`, dispatch to its handler, and run applyBlock. Same error
   * mapping as applyBuffer.
   */
  chain_tx::Roe<void> applyBlock(const Ledger::Record &rec, AccountBuffer &bank,
                                 const BlockApplyContext &ctx) const;

  /** Set per-handler logger names (optional). */
  void redirectLoggers(const std::string &baseName);

private:
  std::array<std::unique_ptr<ITxHandler>, kNumTxTypes> handlers_{};
};

} // namespace pp

