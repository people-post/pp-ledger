#ifndef PP_LEDGER_END_USER_TX_HANDLER_H
#define PP_LEDGER_END_USER_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

class EndUserTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  bool participatesInAccountRenewalValidation() const override { return true; }

  chain_tx::Roe<bool>
  matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const override;

  chain_tx::Roe<std::optional<uint64_t>>
  getRenewalAccountIdIfAny(const Ledger::TypedTx &tx) const override;

  chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

private:
  chain_tx::Roe<void>
  applyEndUser(const Ledger::TxEndUser &tx, const TxContext &ctx,
               AccountBuffer &bank, bool isBufferMode) const;
};

} // namespace pp

#endif
