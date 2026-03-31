#ifndef PP_LEDGER_I_TX_HANDLER_H
#define PP_LEDGER_I_TX_HANDLER_H

#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxIdempotency.h"
#include "TxContext.h"
#include "TxError.h"
#include "../ledger/Ledger.h"
#include "lib/common/Module.h"

#include <optional>
#include <utility>

namespace pp {

/**
 * Per-transaction-type handler (validation + application).
 * Virtuals are added per type; defaults return errors so only the matching
 * handler for a dispatch path needs overrides.
 * Inherits Module so each handler owns its logger (redirect from Chain ctor).
 */
class ITxHandler : public Module {
public:
  ~ITxHandler() override = default;

  /**
   * Whether this tx should be included when blocks are restricted to renewals
   * only (e.g. checkpoint-driven renewal blocks).
   */
  virtual bool isRenewalTx() const { return false; }

  /** True for tx types that must be validated by validateAccountRenewals(). */
  virtual bool participatesInAccountRenewalValidation() const { return false; }

  /**
   * Wallet-indexing predicate used by Chain history queries.
   * Default is false: the tx does not belong to an arbitrary wallet query.
   */
  virtual chain_tx::Roe<bool>
  matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const {
    (void)tx;
    (void)walletId;
    return false;
  }

  /**
   * If a tx participates in idempotency rules, return a (walletId,idempotentId)
   * key. Otherwise return nullopt.
   */
  virtual chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
  getIdempotencyKey(const Ledger::TypedTx &tx) const {
    (void)tx;
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }

  /**
   * If a tx renews (or terminates) an account as part of the renewal system,
   * return the affected account id. Otherwise return nullopt.
   */
  virtual chain_tx::Roe<std::optional<uint64_t>>
  getRenewalAccountIdIfAny(const Ledger::TypedTx &tx) const {
    (void)tx;
    return std::optional<uint64_t>{};
  }

  /**
   * Return the account id whose wallet must have signed this tx.
   * Used by Chain signature validation to avoid hard-coding per-type policies.
   *
   * slotLeaderId is non-zero when validating txs in a normal block.
   * Some tx types (e.g. T_RENEWAL/T_END_USER) may be miner-signed in that case.
   */
  virtual chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const {
    (void)tx;
    (void)slotLeaderId;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getSignerAccountId not implemented for this handler");
  }

  /** Scratch-buffer / mempool path after signature validation. */
  virtual chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const {
    (void)tx;
    (void)bank;
    (void)c;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer not implemented for this handler");
  }

  /** Committed-chain (block replay) path after signature validation. */
  virtual chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const {
    (void)tx;
    (void)bank;
    (void)c;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock not implemented for this handler");
  }

protected:
  /**
   * Cross-block idempotency check using `ctx.idempotencyKeyForRecord` (wired by
   * Chain). Forwards to chain_tx::validateIdempotencyRules.
   */
  chain_tx::Roe<void>
  validateIdempotencyUsingContext(const TxContext &ctx, uint64_t idempotentId,
                                  uint64_t walletIdForIdempotency,
                                  int64_t validationTsMin,
                                  int64_t validationTsMax,
                                  uint64_t effectiveSlot,
                                  bool isStrictMode) const {
    if (!ctx.idempotencyKeyForRecord.has_value()) {
      return chain_tx::TxError(
          chain_err::E_INTERNAL,
          "Idempotency key extractor not configured on TxContext");
    }
    return chain_tx::validateIdempotencyRules(
        ctx.ledger, ctx.consensus, ctx.optChainConfig, idempotentId,
        walletIdForIdempotency, validationTsMin, validationTsMax, effectiveSlot,
        isStrictMode, *ctx.idempotencyKeyForRecord);
  }
};

} // namespace pp

#endif
