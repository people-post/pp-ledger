#ifndef PP_LEDGER_RECOVERY_TX_HANDLER_H
#define PP_LEDGER_RECOVERY_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

/**
 * T_RECOVERY (beacon-authority ba7, B004 / B007): the recovery set announces a new admin set. The
 * record is signed by the recovery set in the chain authority, not by an account (the chain checks
 * that before apply). Applied, it puts the recovery in the authority as pending: the admin set is
 * frozen until the first block of `applyEpoch` (= this block's epoch + `delayEpochs`) replaces it
 * (Chain::enterBlockEpoch). One pending at a time; a block holds at most one.
 */
class RecoveryTxHandler final : public ITxHandler {
public:
  /** Draft tag: one recovery per block (AccountBuffer::addDraftTag). */
  static constexpr const char *kDraftTag = "recovery";

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

  /** The announcement a T_RECOVERY carries (fee must be 0). */
  static chain_tx::Roe<RecoveryAnnouncement> decodeAnnouncement(const Ledger::TxRecovery &tx);

private:
  /** Checks shared by draft and block apply; returns the announcement. */
  chain_tx::Roe<RecoveryAnnouncement> check(const Ledger::TxRecovery &tx, const TxContext &ctx, uint64_t blockId,
                                            uint64_t slot, chain_block::BlockAdmissionMode admissionMode) const;
};

} // namespace pp

#endif
