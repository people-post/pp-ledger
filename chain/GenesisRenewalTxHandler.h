#ifndef PP_LEDGER_GENESIS_RENEWAL_TX_HANDLER_H
#define PP_LEDGER_GENESIS_RENEWAL_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

/** T_RENEWAL when renewing the genesis account (miner-signed). */
class GenesisRenewalTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyGenesisRenewal(const Ledger::TxCommon &tx, const TxContext &ctx,
                      AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
                      bool isStrictMode) override;
};

} // namespace pp

#endif
