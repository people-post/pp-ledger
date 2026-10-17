#include "UserAccountUpsertBase.h"

#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxLedgerMeta.h"
#include "../client/Client.h"

#include <string>

namespace pp {

chain_tx::Roe<void>
UserAccountUpsertBase::applyUserUpdateBlockCommon(
    const Ledger::TxUserUpdate &tx, AccountBuffer &bank,
    const BlockApplyContext &c, UpsertKind kind) const {
  // User-signed updates must carry a non-zero id (0 would skip replay
  // protection). Chain-generated renewals: their id is always 0 and
  // validateAccountRenewals bounds them per block instead.
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, tx.idempotentId, tx.walletId, tx.validationTsMin,
          tx.validationTsMax, c.blockSlot, c.admissionMode, kind == UpsertKind::Update);
      !idem) {
    return idem;
  }
  return applyUserAccountUpsert(tx, c.ctx, bank, c.blockId, false,
                                c.admissionMode, kind);
}

chain_tx::Roe<void>
UserAccountUpsertBase::applyUserUpdateBufferCommon(
    const Ledger::TxUserUpdate &tx, AccountBuffer &bank,
    const BufferApplyContext &c, UpsertKind kind) const {
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, tx.idempotentId, tx.walletId, tx.validationTsMin,
          tx.validationTsMax, c.effectiveSlot, c.admissionMode,
          kind == UpsertKind::Update);
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

  // Preserve existing semantics: buffer-path user-update applies in Full mode.
  return applyUserAccountUpsert(tx, c.ctx, bank, c.blockId, true,
                                chain_block::BlockAdmissionMode::Full, kind);
}

chain_tx::Roe<void> UserAccountUpsertBase::applyUserAccountUpsert(
    const Ledger::TxUserUpdate &tx, const TxContext &ctx, AccountBuffer &bank,
    uint64_t blockId, bool isBufferMode,
    chain_block::BlockAdmissionMode admissionMode, UpsertKind kind) const {
  if (kind == UpsertKind::SystemRenewal) {
    if (tx.fee != 0) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "System account renewal must be fee-free: " + std::to_string(tx.walletId));
    }
  } else if (chain_block::admissionTxStrict(admissionMode)) {
    if (auto feeGate = chain_tx::requireMinimumFee(
            ctx.optChainConfig, ctx.fnBillableCustomMetaSizeForFee,
            Ledger::TypedTx(tx), tx.fee,
            "Chain config required for Full-mode user-update fee validation",
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
      tx.walletId, deserializeError, chain_block::admissionTxStrict(admissionMode));
  if (!userAccountRoe) {
    return userAccountRoe.error();
  }
  Client::UserAccount userAccount = std::move(userAccountRoe.value());

  auto bufferAccountResult = bank.getAccount(tx.walletId);
  if (!bufferAccountResult) {
    if (chain_block::admissionTxStrict(admissionMode)) {
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
    // Miner state is chain-owned: an update or renewal carries it unchanged (only
    // T_MINER_REGISTER / T_MINER_REVOKE and block apply change it).
    if (userAccount.wallet.miner != bufferAccountResult.value().wallet.miner) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Account update cannot change miner state: " + std::to_string(tx.walletId));
    }
    if (kind != UpsertKind::Update && chain_block::admissionTxStrict(admissionMode)) {
      if (auto kept = checkRenewalKeepsAccount(userAccount, bufferAccountResult.value()); !kept) {
        return kept;
      }
    }
  }

  if (auto replaced = chain_tx::replaceAccount(
          bank, tx.walletId, blockId, userAccount.wallet, userAccount.meta,
          "Failed to add user account to buffer: ");
      !replaced) {
    return replaced;
  }

  return chain_tx::creditFeeToFeeAccount(
      bank, tx.fee, "Failed to credit fee to fee account: ");
}

chain_tx::Roe<void> UserAccountUpsertBase::checkRenewalKeepsAccount(const Client::UserAccount &carried,
                                                                    const AccountBuffer::Account &held) const {
  // The slot leader signs renewals: it may re-record an account, never choose its keys or meta.
  const std::string who = std::to_string(held.id);
  if (carried.wallet.publicKeys != held.wallet.publicKeys || carried.wallet.minSignatures != held.wallet.minSignatures ||
      carried.wallet.keyType != held.wallet.keyType) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Renewal cannot change the keys of account " + who);
  }
  // The meta in force is the one the account's last write recorded (what renewal builds from); the
  // account holds its hash (B031), so no block read is needed.
  if (AccountBuffer::attachmentHashOf(carried.meta) != held.attachmentHash) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Renewal cannot change the meta of account " + who);
  }
  return {};
}

} // namespace pp
