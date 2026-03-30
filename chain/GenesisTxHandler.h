#ifndef PP_LEDGER_GENESIS_TX_HANDLER_H
#define PP_LEDGER_GENESIS_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class GenesisTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyGenesisInit(const Ledger::TxCommon &tx,
                   TxContext &ctx) override;
};

} // namespace pp

#endif
