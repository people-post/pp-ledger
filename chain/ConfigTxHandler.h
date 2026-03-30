#ifndef PP_LEDGER_CONFIG_TX_HANDLER_H
#define PP_LEDGER_CONFIG_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class ConfigTxHandler final : public ITxHandler {
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
  applyConfigUpdate(const Ledger::TxConfig &tx, const TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId,
                    bool isStrictMode);

  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::TxConfig &tx, TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
                    bool commitOptChainConfig);
};

} // namespace pp

#endif
