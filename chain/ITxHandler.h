#ifndef PP_LEDGER_I_TX_HANDLER_H
#define PP_LEDGER_I_TX_HANDLER_H

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
};

} // namespace pp

#endif
