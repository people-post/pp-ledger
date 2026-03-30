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
              const BufferApplyContext &c) const {
    (void)tx;
    (void)bank;
    (void)c;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer not implemented for this handler");
  }

  /** Committed-chain (block replay) path after signature validation. */
  virtual chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const {
    (void)tx;
    (void)bank;
    (void)c;
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock not implemented for this handler");
  }
};

} // namespace pp

#endif
