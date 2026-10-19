#ifndef PP_LEDGER_END_USER_TX_HANDLER_H
#define PP_LEDGER_END_USER_TX_HANDLER_H

#include "ITxHandler.h"

#include <map>

namespace pp {

/**
 * T_END_USER: an account that cannot pay its renewal is closed; its balances go to the recycle
 * account. The record's meta carries those balances (beacon-authority ba5b), so a node warming up
 * from a checkpoint, which may not hold the account, still credits the recycle account exactly.
 */
class EndUserTxHandler final : public ITxHandler {
public:
  /** The record's meta for an account with `balances`. */
  static std::string writtenOffMeta(const std::map<uint64_t, int64_t> &balances);

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
               AccountBuffer &bank, chain_block::BlockAdmissionMode admissionMode) const;
  /** The account's native balance is below its renewal fee (why it is ended). */
  chain_tx::Roe<void> checkCannotPayRenewal(const Ledger::TxEndUser &tx, const TxContext &ctx,
                                            const AccountBuffer &bank) const;
  /** Credit a held recycle account with the positive `balances`. */
  static chain_tx::Roe<void> creditRecycle(AccountBuffer &bank, const std::map<uint64_t, int64_t> &balances);
};

} // namespace pp

#endif
