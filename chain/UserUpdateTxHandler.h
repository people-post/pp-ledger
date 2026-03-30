#ifndef PP_LEDGER_USER_UPDATE_TX_HANDLER_H
#define PP_LEDGER_USER_UPDATE_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class UserUpdateTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) override;

  chain_tx::Roe<void>
  applyUserAccountUpsert(const Ledger::TxUserUpdate &tx,
                         const TxContext &ctx, AccountBuffer &bank,
                         uint64_t blockId, bool isBufferMode,
                         bool isStrictMode) override;
};

} // namespace pp

#endif
