#ifndef PP_LEDGER_CONFIG_TX_HANDLER_H
#define PP_LEDGER_CONFIG_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class ConfigTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<void>
  applyBuffer(const TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) override;

  chain_tx::Roe<void>
  applyBlock(const TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) override;

  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::TxConfig &tx, const TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId,
                    bool isStrictMode) override;

  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::TxConfig &tx, TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
                    bool commitOptChainConfig) override;
};

} // namespace pp

#endif
