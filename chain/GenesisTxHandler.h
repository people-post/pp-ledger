#ifndef PP_LEDGER_GENESIS_TX_HANDLER_H
#define PP_LEDGER_GENESIS_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class GenesisTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<bool>
  matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const override;

  chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

  std::optional<std::string>
  getGenesisAccountMetaForTx(const Ledger::TypedTx &tx,
                             const Ledger::Block &block) const override;

private:
  chain_tx::Roe<void> applyGenesisInit(const Ledger::TxGenesis &tx,
                                      TxContext &ctx) const;
};

} // namespace pp

#endif
