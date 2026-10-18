#include "AccountRecoveryTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxTyped.h"

#include <limits>
#include <string>

namespace pp {

chain_tx::Roe<uint64_t> AccountRecoveryTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                                                     uint64_t /*slotLeaderId*/) const {
  // Indexed under the recovered account; the signatures are its recovery set's (Chain checks them).
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "getSignerAccountId", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  return p.value()->walletId;
}

chain_tx::Roe<bool> AccountRecoveryTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                                                    uint64_t walletId) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "matchesWalletForIndex", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  return p.value()->walletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
AccountRecoveryTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "getIdempotencyKey", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  if (p.value()->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(std::make_pair(p.value()->walletId, p.value()->idempotentId));
}

chain_tx::Roe<AccountRecoveryAnnouncement>
AccountRecoveryTxHandler::decodeAnnouncement(const Ledger::TxAccountRecovery &tx) {
  AccountRecoveryAnnouncement announcement;
  if (!announcement.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE, "Failed to deserialize the account recovery");
  }
  if (auto valid = chain_tx::validateRecoveryKeys(announcement.publicKeys, announcement.minSignatures,
                                                  "The announced keys");
      !valid) {
    return valid.error();
  }
  return announcement;
}

chain_tx::Roe<void> AccountRecoveryTxHandler::applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                          const BufferApplyContext &c) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "applyBuffer", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  const auto &r = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, r.idempotentId, r.walletId, r.validationTsMin,
                                                  r.validationTsMax, c.effectiveSlot, c.admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  if (auto seeded = chain_tx::seedCommittedAccount(bank, c.ctx.bank, r.walletId); !seeded) {
    return seeded;
  }
  if (auto seeded = chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, r.fee); !seeded) {
    return seeded;
  }
  return apply(r, c.ctx, bank, c.blockId, c.effectiveSlot, chain_block::BlockAdmissionMode::Full);
}

chain_tx::Roe<void> AccountRecoveryTxHandler::applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                         const BlockApplyContext &c) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "applyBlock", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  const auto &r = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, r.idempotentId, r.walletId, r.validationTsMin,
                                                  r.validationTsMax, c.blockSlot, c.admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  return apply(r, c.ctx, bank, c.blockId, c.blockSlot, c.admissionMode);
}

chain_tx::Roe<void> AccountRecoveryTxHandler::apply(const Ledger::TxAccountRecovery &tx, const TxContext &ctx,
                                                    AccountBuffer &bank, uint64_t blockId, uint64_t slot,
                                                    chain_block::BlockAdmissionMode admissionMode) const {
  if (blockId == 0) {
    return chain_tx::TxError(chain_err::E_TX_TYPE, "An account recovery cannot be in the genesis block");
  }
  auto announcement = decodeAnnouncement(tx);
  if (!announcement) {
    return announcement.error();
  }
  if (chain_block::admissionTxStrict(admissionMode)) {
    if (!ctx.optChainConfig.has_value() || !ctx.fnBillableCustomMetaSizeForFee.has_value()) {
      return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain config required for an account recovery");
    }
    auto least = chain_tx::calculateMinimumFeeForTransaction(ctx.optChainConfig.value(), Ledger::TypedTx(tx),
                                                             *ctx.fnBillableCustomMetaSizeForFee);
    if (!least) {
      return least.error();
    }
    if (tx.fee < least.value()) {
      return chain_tx::TxError(chain_err::E_TX_FEE, "Account recovery fee below minimum " +
                                                        std::to_string(least.value()) + ": " + std::to_string(tx.fee));
    }
  }
  auto accountRoe = bank.getAccount(tx.walletId);
  if (!accountRoe) {
    if (!chain_block::admissionTxStrict(admissionMode)) {
      // Warming up: the account arrives with its renewal; the fee still reaches a held fee account.
      return chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit the recovery fee: ");
    }
    return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND, "Account not found: " + std::to_string(tx.walletId));
  }
  Client::AccountRecovery recovery = accountRoe.value().wallet.recovery;
  if (!recovery.hasSet()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Account " + std::to_string(tx.walletId) + " has no recovery set");
  }
  if (recovery.pending) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Account " + std::to_string(tx.walletId) +
                                                             " has a recovery announced already (until epoch " +
                                                             std::to_string(recovery.applyEpoch) + ")");
  }
  if (tx.fee > 0) {
    if (auto paid = bank.withdrawBalance(tx.walletId, AccountBuffer::ID_GENESIS, static_cast<int64_t>(
                                                                                     std::min<uint64_t>(tx.fee, INT64_MAX)));
        !paid) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_BALANCE, "The account cannot pay the recovery's fee: " +
                                                                 paid.error().message);
    }
    if (auto credited = chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit the recovery fee: ");
        !credited) {
      return credited;
    }
  }
  recovery.pending = true;
  recovery.nextPublicKeys = announcement.value().publicKeys;
  recovery.nextMinSignatures = announcement.value().minSignatures;
  recovery.applyEpoch = ctx.consensus.getEpochFromSlot(slot) + recovery.delayEpochs;
  recovery.announcedAt = blockId;
  if (auto set = bank.setRecovery(tx.walletId, recovery); !set) {
    return chain_tx::TxError(chain_err::E_INTERNAL_BUFFER, set.error().message);
  }
  return {};
}

} // namespace pp
