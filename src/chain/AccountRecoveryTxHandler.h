#ifndef PP_LEDGER_ACCOUNT_RECOVERY_TX_HANDLER_H
#define PP_LEDGER_ACCOUNT_RECOVERY_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

/**
 * T_ACCOUNT_RECOVERY (projects/account-recovery): an account's recovery set announces its next keys.
 * Signed by the account's recovery set, not its keys (Chain checks that before apply). The account has
 * a set and no announcement waiting; the fee (meta-size) comes from its native balance. Applied, the
 * announcement waits in the account's recovery state — in place, as miner state — until the first
 * block of an epoch at or past `applyEpoch` (= this block's epoch + the account's delay) applies it
 * (AccountBuffer::applyDueRecoveries), unless the account's own update clears it first.
 */
class AccountRecoveryTxHandler final : public ITxHandler {
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

  /** The announcement a T_ACCOUNT_RECOVERY carries, its keys checked. */
  static chain_tx::Roe<AccountRecoveryAnnouncement> decodeAnnouncement(const Ledger::TxAccountRecovery &tx);

private:
  chain_tx::Roe<void> apply(const Ledger::TxAccountRecovery &tx, const TxContext &ctx, AccountBuffer &bank,
                            uint64_t blockId, uint64_t slot, chain_block::BlockAdmissionMode admissionMode) const;
};

} // namespace pp

#endif
