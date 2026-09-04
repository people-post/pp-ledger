#include "UserAccountUpsertBase.h"

#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "../client/Client.h"

#include <string>

namespace pp {

chain_tx::Roe<void>
UserAccountUpsertBase::applyUserUpdateBlockCommon(
    const Ledger::TxUserUpdate &tx, AccountBuffer &bank,
    const BlockApplyContext &c) const {
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, tx.idempotentId, tx.walletId, tx.validationTsMin,
          tx.validationTsMax, c.blockSlot, c.isStrictMode);
      !idem) {
    return idem;
  }
  return applyUserAccountUpsert(tx, c.ctx, bank, c.blockId, false,
                                c.isStrictMode);
}

chain_tx::Roe<void>
UserAccountUpsertBase::applyUserUpdateBufferCommon(
    const Ledger::TxUserUpdate &tx, AccountBuffer &bank,
    const BufferApplyContext &c) const {
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, tx.idempotentId, tx.walletId, tx.validationTsMin,
          tx.validationTsMax, c.effectiveSlot, c.isStrictMode);
      !idem) {
    return idem;
  }

  if (auto seeded =
          chain_tx::seedCommittedAccount(bank, c.ctx.bank, tx.walletId);
      !seeded) {
    return seeded;
  }
  if (tx.walletId != AccountBuffer::ID_FEE) {
    if (auto seeded =
            chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, tx.fee);
        !seeded) {
      return seeded;
    }
  }

  // Preserve existing semantics: buffer-path user-update applies in strict mode.
  return applyUserAccountUpsert(tx, c.ctx, bank, c.blockId, true, true);
}

chain_tx::Roe<void> UserAccountUpsertBase::applyUserAccountUpsert(
    const Ledger::TxUserUpdate &tx, const TxContext &ctx, AccountBuffer &bank,
    uint64_t blockId, bool isBufferMode, bool isStrictMode) const {
  if (isStrictMode) {
    if (auto feeGate = chain_tx::requireMinimumFee(
            ctx.optChainConfig, ctx.fnBillableCustomMetaSizeForFee,
            Ledger::TypedTx(tx), tx.fee,
            "Chain config required for strict user-update fee validation",
            "User update transaction fee below minimum: ");
        !feeGate) {
      return feeGate;
    }
  }

  const std::string deserializeError =
      "Failed to deserialize user meta for account " +
      std::to_string(tx.walletId) + ": " + std::to_string(tx.meta.size()) +
      " bytes";
  auto userAccountRoe = chain_tx::loadAndValidateUserAccountMeta(
      tx.meta, ctx.crypto, bank, isBufferMode ? &ctx.bank : nullptr,
      tx.walletId, deserializeError);
  if (!userAccountRoe) {
    return userAccountRoe.error();
  }
  Client::UserAccount userAccount = std::move(userAccountRoe.value());

  auto bufferAccountResult = bank.getAccount(tx.walletId);
  if (!bufferAccountResult) {
    if (isStrictMode) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND,
                               "User account not found in buffer: " +
                                   std::to_string(tx.walletId));
    }
  } else {
    auto balanceVerifyResult = bank.verifyBalance(
        tx.walletId, 0, tx.fee, userAccount.wallet.mBalances);
    if (!balanceVerifyResult) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               balanceVerifyResult.error().message);
    }
  }

  if (auto replaced = chain_tx::replaceAccount(
          bank, tx.walletId, blockId, userAccount.wallet,
          "Failed to add user account to buffer: ");
      !replaced) {
    return replaced;
  }

  return chain_tx::creditFeeToFeeAccount(
      bank, tx.fee, "Failed to credit fee to fee account: ");
}

} // namespace pp
