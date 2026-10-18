#ifndef PP_LEDGER_ACCOUNT_RECOVERY_TX_HANDLER_H
#define PP_LEDGER_ACCOUNT_RECOVERY_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

/**
 * T_ACCOUNT_RECOVERY (projects/account-recovery): an account's recovery set acts on it — announces its
 * next keys, withdraws that announcement, or changes itself (AccountRecoveryAction). Signed by the
 * account's recovery set, not its keys (Chain checks that before apply); the account pays the base
 * fee from its native balance. An announcement waits in the account's recovery state — in place, as
 * miner state — with the account frozen (RecordHandler refuses what its keys sign) until the first
 * block of `applyEpoch` (recoveryApplyEpoch) applies it (AccountBuffer::applyDueRecoveries).
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

  /** The action a T_ACCOUNT_RECOVERY carries: a known kind, its keys checked for an announcement. */
  static chain_tx::Roe<AccountRecoveryAction> decodeAction(const Ledger::TxAccountRecovery &tx);

private:
  chain_tx::Roe<void> apply(const Ledger::TxAccountRecovery &tx, const TxContext &ctx, AccountBuffer &bank,
                            uint64_t blockId, uint64_t slot, chain_block::BlockAdmissionMode admissionMode) const;
};

} // namespace pp

#endif
