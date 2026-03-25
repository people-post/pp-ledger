#ifndef PP_LEDGER_END_USER_TX_HANDLER_H
#define PP_LEDGER_END_USER_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class EndUserTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyEndUser(const Ledger::Transaction &tx, const TxContext &ctx,
               AccountBuffer &bank, bool isBufferMode) override;
};

} // namespace pp

#endif
