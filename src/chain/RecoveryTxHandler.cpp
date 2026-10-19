#include "chain/RecoveryTxHandler.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/AccountPolicy.h"
#include "ledger/chain/ErrorCodes.h"
#include "chain/TxTyped.h"
#include "ledger/chain/Types.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t> RecoveryTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                                              uint64_t /*slotLeaderId*/) const {
  // Indexed under the genesis account (the admin set it replaces); signatures are the recovery set's.
  auto pRoe = chain_tx::expectTx<Ledger::TxRecovery>(tx, "getSignerAccountId", "TxRecovery");
  if (!pRoe) {
    return pRoe.error();
  }
  return AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<bool> RecoveryTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxRecovery>(tx, "matchesWalletForIndex", "TxRecovery");
  if (!pRoe) {
    return pRoe.error();
  }
  return walletId == AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
RecoveryTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxRecovery>(tx, "getIdempotencyKey", "TxRecovery");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(std::make_pair(AccountBuffer::ID_GENESIS, p->idempotentId));
}

chain_tx::Roe<RecoveryAnnouncement> RecoveryTxHandler::decodeAnnouncement(const Ledger::TxRecovery &tx) {
  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Recovery transaction must have fee 0");
  }
  RecoveryAnnouncement announcement;
  if (!announcement.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE, "Failed to deserialize recovery announcement");
  }
  return announcement;
}

chain_tx::Roe<RecoveryAnnouncement> RecoveryTxHandler::check(const Ledger::TxRecovery &tx, const TxContext &ctx,
                                                             uint64_t blockId, uint64_t slot,
                                                             chain_block::BlockAdmissionMode admissionMode) const {
  if (blockId == 0) {
    return chain_tx::TxError(chain_err::E_TX_TYPE, "A recovery cannot be announced in the genesis block");
  }
  if (auto idem = validateIdempotencyUsingContext(ctx, tx.idempotentId, AccountBuffer::ID_GENESIS,
                                                  tx.validationTsMin, tx.validationTsMax, slot, admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem.error();
  }
  auto announcement = decodeAnnouncement(tx);
  if (!announcement) {
    return announcement.error();
  }
  if (!ctx.optAuthority.has_value() || !ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain authority not initialized");
  }
  if (auto valid = chain_tx::validateRecoveryAnnouncement(announcement.value(), ctx.optAuthority.value(),
                                                         ctx.optChainConfig->networkId);
      !valid) {
    return valid.error();
  }
  return announcement;
}

chain_tx::Roe<void> RecoveryTxHandler::applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                   const BufferApplyContext &c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxRecovery>(tx, "applyBuffer", "TxRecovery");
  if (!pRoe) {
    return pRoe.error();
  }
  // The authority changes only when a block applies: a second announcement would pass here.
  if (bank.hasDraftTag(kDraftTag)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "A block holds at most one recovery announcement");
  }
  auto announcement = check(*pRoe.value(), c.ctx, c.blockId, c.effectiveSlot, c.admissionMode);
  if (!announcement) {
    return announcement.error();
  }
  bank.addDraftTag(kDraftTag);
  return {};
}

chain_tx::Roe<void> RecoveryTxHandler::applyBlock(const Ledger::TypedTx &tx, AccountBuffer & /*bank*/,
                                                  const BlockApplyContext &c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxRecovery>(tx, "applyBlock", "TxRecovery");
  if (!pRoe) {
    return pRoe.error();
  }
  auto announcement = check(*pRoe.value(), c.ctx, c.blockId, c.blockSlot, c.admissionMode);
  if (!announcement) {
    return announcement.error();
  }
  auto &authority = c.ctx.optAuthority.value();
  authority.hasPendingRecovery = true;
  authority.pendingRecovery.announcement = announcement.value();
  authority.pendingRecovery.announcedAt = c.blockId;
  authority.pendingRecovery.applyEpoch = c.ctx.consensus.getEpochFromSlot(c.blockSlot) + authority.recovery.delayEpochs;
  log().error << "RECOVERY ANNOUNCED at block " << c.blockId << ": the recovery set replaces the admin set with "
              << announcement.value().adminPublicKeys.size() << " keys ("
              << static_cast<int>(announcement.value().adminMinSignatures) << " signatures) at epoch "
              << authority.pendingRecovery.applyEpoch << "; the admin set is frozen until then";
  return {};
}

} // namespace pp
