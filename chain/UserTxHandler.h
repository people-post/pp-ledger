#ifndef PP_LEDGER_USER_TX_HANDLER_H
#define PP_LEDGER_USER_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class UserTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyUserAccountUpsert(const Ledger::TxUser &tx,
                         const TxContext &ctx, AccountBuffer &bank,
                         uint64_t blockId, bool isBufferMode,
                         bool isStrictMode) override;
};

} // namespace pp

#endif
