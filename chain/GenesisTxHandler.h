#ifndef PP_LEDGER_GENESIS_TX_HANDLER_H
#define PP_LEDGER_GENESIS_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class GenesisTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) override;

  chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) override;

  chain_tx::Roe<void>
  applyGenesisInit(const Ledger::TxGenesis &tx,
                   TxContext &ctx) override;
};

} // namespace pp

#endif
