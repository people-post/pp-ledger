#ifndef PP_LEDGER_I_TX_HANDLER_H
#define PP_LEDGER_I_TX_HANDLER_H

#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxContext.h"
#include "TxError.h"
#include "../ledger/Ledger.h"

namespace pp {

/**
 * Per-transaction-type handler (validation + application).
 * Virtuals are added per type; defaults return errors so only the matching
 * handler for a dispatch path needs overrides.
 */
class ITxHandler {
public:
  virtual ~ITxHandler() = default;

  /** T_GENESIS checkpoint tx after signature validation (genesis block only). */
  virtual chain_tx::Roe<void>
  applyGenesisInit(const Ledger::Transaction &tx,
                   ChainTxContext &ctx) {
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
  applyConfigUpdate(const Ledger::Transaction &tx, ChainTxContextConst &ctx,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode) {
    (void)tx;
    (void)ctx;
    (void)bank;
    (void)blockId;
    (void)isStrictMode;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyConfigUpdate(ChainTxContextConst&) not implemented for this handler");
  }

  /**
   * T_CONFIG: chain `bank_` path. When commitOptChainConfig, writes meta config
   * into ctx.optChainConfig.
   */
  virtual chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::Transaction &tx, ChainTxContext &ctx,
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
        "applyConfigUpdate(ChainTxContext&) not implemented for this handler");
  }

  /**
   * T_NEW_USER: fund and register a new account. `bank` is the working
   * buffer; `ctx.bank` is committed chain state (for buffer-mode existence).
   * When isBufferMode, `fromWalletId` must already be present in `bank`
   * (e.g. via ensureAccountInBuffer).
   */
  virtual chain_tx::Roe<void>
  applyNewUser(const Ledger::Transaction &tx, ChainTxContextConst &ctx,
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
   * T_USER: replace account meta (and pay fee). Same semantics as
   * applyNewUser for `bank` / `ctx.bank` / isBufferMode (caller ensures
   * fromWalletId and fee account in buffer when isBufferMode).
   */
  virtual chain_tx::Roe<void>
  applyUserAccountUpsert(const Ledger::Transaction &tx,
                         ChainTxContextConst &ctx, AccountBuffer &bank,
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
  applyDefaultTransferStrict(const Ledger::Transaction &tx,
                             ChainTxContextConst &ctx, AccountBuffer &bank) {
    (void)tx;
    (void)ctx;
    (void)bank;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyDefaultTransferStrict not implemented for this handler");
  }

  /** T_DEFAULT loose path: tolerates missing from/to accounts. */
  virtual chain_tx::Roe<void>
  applyDefaultTransferLoose(const Ledger::Transaction &tx,
                            ChainTxContextConst &ctx, AccountBuffer &bank) {
    (void)tx;
    (void)ctx;
    (void)bank;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyDefaultTransferLoose not implemented for this handler");
  }

  /**
   * T_RENEWAL for genesis only (ID_GENESIS -> ID_GENESIS). User renewals use
   * applyUserAccountUpsert via T_USER handler from Chain.
   */
  virtual chain_tx::Roe<void>
  applyGenesisRenewal(const Ledger::Transaction &tx, ChainTxContextConst &ctx,
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
  applyEndUser(const Ledger::Transaction &tx, ChainTxContextConst &ctx,
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
