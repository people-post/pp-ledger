#ifndef PP_LEDGER_DEFAULT_TX_HANDLER_H
#define PP_LEDGER_DEFAULT_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class DefaultTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

private:
  chain_tx::Roe<void>
  applyDefaultTransferStrict(const Ledger::TxDefault &tx,
                             const TxContext &ctx,
                             AccountBuffer &bank) const;

  chain_tx::Roe<void>
  applyDefaultTransferLoose(const Ledger::TxDefault &tx,
                            const TxContext &ctx,
                            AccountBuffer &bank) const;
};

} // namespace pp

#endif
