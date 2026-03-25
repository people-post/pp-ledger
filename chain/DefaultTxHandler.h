#ifndef PP_LEDGER_DEFAULT_TX_HANDLER_H
#define PP_LEDGER_DEFAULT_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class DefaultTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyDefaultTransferStrict(const Ledger::Transaction &tx,
                             ChainTxContextConst &ctx,
                             AccountBuffer &bank) override;

  chain_tx::Roe<void>
  applyDefaultTransferLoose(const Ledger::Transaction &tx,
                            ChainTxContextConst &ctx,
                            AccountBuffer &bank) override;
};

} // namespace pp

#endif
