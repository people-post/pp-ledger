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
   * T_CONFIG: genesis-signed system config / genesis account update.
   * @param chainConfigBaseline Current chain config for strict-mode checks (may be empty).
   * @param bank Account buffer to mutate (chain `bank_` or a scratch buffer).
   * @param commitOptChainConfig When true, write meta config to *commitTarget (non-null).
   */
  virtual chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::Transaction &tx, logging::Logger &logger,
                    const std::optional<BlockChainConfig> &chainConfigBaseline,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
                    bool commitOptChainConfig,
                    std::optional<BlockChainConfig> *commitTarget) {
    (void)tx;
    (void)logger;
    (void)chainConfigBaseline;
    (void)bank;
    (void)blockId;
    (void)isStrictMode;
    (void)commitOptChainConfig;
    (void)commitTarget;
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "applyConfigUpdate not implemented for this handler");
  }
};

} // namespace pp

#endif
