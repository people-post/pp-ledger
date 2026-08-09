#ifndef PP_LEDGER_DEFAULT_TX_HANDLER_H
#define PP_LEDGER_DEFAULT_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class DefaultTxHandler final : public ITxHandler {
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
