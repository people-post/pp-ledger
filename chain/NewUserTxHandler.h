#ifndef PP_LEDGER_NEW_USER_TX_HANDLER_H
#define PP_LEDGER_NEW_USER_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class NewUserTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyNewUser(const Ledger::TxNewUser &tx, const TxContext &ctx,
               AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
               bool isStrictMode) override;
};

} // namespace pp

#endif
