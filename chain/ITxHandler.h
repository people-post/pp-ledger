#ifndef PP_LEDGER_I_TX_HANDLER_H
#define PP_LEDGER_I_TX_HANDLER_H

#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxContext.h"
#include "TxError.h"
#include "../ledger/Ledger.h"
#include "../ledger/TypedTx.h"
#include "lib/common/Module.h"

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
   * Return the account id whose wallet must have signed this tx.
   * Used by Chain signature validation to avoid hard-coding per-type policies.
   *
   * slotLeaderId is non-zero when validating txs in a normal block.
   * Some tx types (e.g. T_RENEWAL/T_END_USER) may be miner-signed in that case.
   */
  virtual chain_tx::Roe<uint64_t>
  getSignerAccountId(const TypedTx &tx, uint64_t slotLeaderId) const {
    (void)tx;
    (void)slotLeaderId;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getSignerAccountId not implemented for this handler");
  }

  /** Scratch-buffer / mempool path after signature validation. */
  virtual chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) {
    (void)tx;
    (void)bank;
    (void)c;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer not implemented for this handler");
  }

  /** Committed-chain (block replay) path after signature validation. */
  virtual chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) {
    (void)tx;
    (void)bank;
    (void)c;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock not implemented for this handler");
  }

  /** T_GENESIS checkpoint tx after signature validation (genesis block only). */
  virtual chain_tx::Roe<void>
  applyGenesisInit(const Ledger::TxGenesis &tx,
                   TxContext &ctx) {
    (void)tx;
    (void)ctx;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyGenesisInit not implemented for this handler");
  }

  /**
   * T_CONFIG: scratch-buffer path (e.g. addBufferTransaction). Const chain view;
   * does not commit `optChainConfig`.
   */
  virtual chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::TxConfig &tx, const TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode) {
    (void)tx;
    (void)ctx;
    (void)bank;
    (void)blockId;
    (void)isStrictMode;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyConfigUpdate(const TxContext&) not implemented for this handler");
  }

  /**
   * T_CONFIG: committed chain bank (Chain's TxContext::bank). When
   * commitOptChainConfig, writes meta config
   * into ctx.optChainConfig.
   */
  virtual chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::TxConfig &tx, TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
                    bool commitOptChainConfig) {
    (void)tx;
    (void)ctx;
    (void)bank;
    (void)blockId;
    (void)isStrictMode;
    (void)commitOptChainConfig;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyConfigUpdate(TxContext&) not implemented for this handler");
  }

  /**
   * T_NEW_USER: fund and register a new account. `bank` is the working
   * buffer; `ctx.bank` is committed chain state (for buffer-mode existence).
   * When isBufferMode, `fromWalletId` must already be present in `bank`
   * (e.g. via AccountBuffer::seedFromCommittedIfMissing from committed ctx.bank).
   */
  virtual chain_tx::Roe<void>
  applyNewUser(const Ledger::TxNewUser &tx, const TxContext &ctx,
               AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
               bool isStrictMode) {
    (void)tx;
    (void)ctx;
    (void)bank;
    (void)blockId;
    (void)isBufferMode;
    (void)isStrictMode;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyNewUser not implemented for this handler");
  }

  /**
   * T_USER_UPDATE: replace account meta (and pay fee). Same semantics as
   * applyNewUser for `bank` / `ctx.bank` / isBufferMode (caller ensures
   * fromWalletId and fee account in buffer when isBufferMode).
   */
  virtual chain_tx::Roe<void>
  applyUserAccountUpsert(const Ledger::TxUserUpdate &tx,
                         const TxContext &ctx, AccountBuffer &bank,
                         uint64_t blockId, bool isBufferMode,
                         bool isStrictMode) {
    (void)tx;
    (void)ctx;
    (void)bank;
    (void)blockId;
    (void)isBufferMode;
    (void)isStrictMode;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyUserAccountUpsert not implemented for this handler");
  }

  /** T_DEFAULT strict path: min fee from config, then transfer. */
  virtual chain_tx::Roe<void>
  applyDefaultTransferStrict(const Ledger::TxDefault &tx,
                             const TxContext &ctx, AccountBuffer &bank) {
    (void)tx;
    (void)ctx;
    (void)bank;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyDefaultTransferStrict not implemented for this handler");
  }

  /** T_DEFAULT loose path: tolerates missing from/to accounts. */
  virtual chain_tx::Roe<void>
  applyDefaultTransferLoose(const Ledger::TxDefault &tx,
                            const TxContext &ctx, AccountBuffer &bank) {
    (void)tx;
    (void)ctx;
    (void)bank;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyDefaultTransferLoose not implemented for this handler");
  }

  /**
   * T_RENEWAL for genesis only (ID_GENESIS -> ID_GENESIS). User renewals use
   * applyUserAccountUpsert via T_USER_UPDATE handler from Chain.
   */
  virtual chain_tx::Roe<void>
  applyGenesisRenewal(const Ledger::TxRenewal &tx, const TxContext &ctx,
                      AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
                      bool isStrictMode) {
    (void)tx;
    (void)ctx;
    (void)bank;
    (void)blockId;
    (void)isBufferMode;
    (void)isStrictMode;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyGenesisRenewal not implemented for this handler");
  }

  /** T_END_USER: write-off when balance below min fee for current meta. */
  virtual chain_tx::Roe<void>
  applyEndUser(const Ledger::TxEndUser &tx, const TxContext &ctx,
               AccountBuffer &bank, bool isBufferMode) {
    (void)tx;
    (void)ctx;
    (void)bank;
    (void)isBufferMode;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyEndUser not implemented for this handler");
  }
};

} // namespace pp

#endif
