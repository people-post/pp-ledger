#ifndef PP_LEDGER_CONFIG_TX_HANDLER_H
#define PP_LEDGER_CONFIG_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class ConfigTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::Transaction &tx, ChainTxContextConst &ctx,
                    AccountBuffer &bank, uint64_t blockId,
                    bool isStrictMode) override;

  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::Transaction &tx, ChainTxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
                    bool commitOptChainConfig) override;
};

} // namespace pp

#endif
