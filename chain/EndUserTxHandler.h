#ifndef PP_LEDGER_END_USER_TX_HANDLER_H
#define PP_LEDGER_END_USER_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class EndUserTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) override;

  chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) override;

private:
  chain_tx::Roe<void>
  applyEndUser(const Ledger::TxEndUser &tx, const TxContext &ctx,
               AccountBuffer &bank, bool isBufferMode);
};

} // namespace pp

#endif
