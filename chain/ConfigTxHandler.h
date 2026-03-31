#ifndef PP_LEDGER_CONFIG_TX_HANDLER_H
#define PP_LEDGER_CONFIG_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class ConfigTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<bool>
  matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const override;

  chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
  getIdempotencyKey(const Ledger::TypedTx &tx) const override;

  chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

  std::optional<std::string>
  genesisAccountMetaForTx(const Ledger::TypedTx &tx,
                          const Ledger::Block &block) const override;

private:
  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::TxConfig &tx, const TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId,
                    bool isStrictMode) const;

  chain_tx::Roe<void>
  applyConfigUpdate(const Ledger::TxConfig &tx, TxContext &ctx,
                    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
                    bool commitOptChainConfig) const;
};

} // namespace pp

#endif
