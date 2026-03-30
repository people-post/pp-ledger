#ifndef PP_LEDGER_DEFAULT_TX_HANDLER_H
#define PP_LEDGER_DEFAULT_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class DefaultTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyDefaultTransferStrict(const Ledger::TxDefault &tx,
                             const TxContext &ctx,
                             AccountBuffer &bank) override;

  chain_tx::Roe<void>
  applyDefaultTransferLoose(const Ledger::TxDefault &tx,
                            const TxContext &ctx,
                            AccountBuffer &bank) override;
};

} // namespace pp

#endif
