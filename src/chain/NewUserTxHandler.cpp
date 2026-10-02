#include "NewUserTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxTyped.h"
#include "../client/Client.h"

#include <string>
#include <limits>
#include <variant>

namespace pp {

namespace {

/**
 * Minimum fee: the meta-size fee, plus the flat newAccountFee for accounts
 * created after genesis (genesis records pay the exact meta-size fee).
 */
chain_tx::Roe<void> requireNewAccountFee(const Ledger::TxNewUser &tx, const TxContext &ctx, uint64_t blockId) {
  if (!ctx.optChainConfig.has_value() || !ctx.fnBillableCustomMetaSizeForFee.has_value()) {
    return chain_tx::TxError(chain_err::E_INTERNAL, "Chain config required for Full-mode new-user fee validation");
  }
  const BlockChainConfig &config = ctx.optChainConfig.value();
  auto metaFee = chain_tx::calculateMinimumFeeForTransaction(config, Ledger::TypedTx(tx),
                                                             *ctx.fnBillableCustomMetaSizeForFee);
  if (!metaFee) {
    return metaFee.error();
  }
  const uint64_t flat = blockId != 0 ? config.newAccountFee : 0;
  if (flat > std::numeric_limits<uint64_t>::max() - metaFee.value()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "New account fee overflows");
  }
  const uint64_t minimum = metaFee.value() + flat;
  if (tx.fee < minimum) {
    return chain_tx::TxError(chain_err::E_TX_FEE, "New user transaction fee below minimum " + std::to_string(minimum) +
                                                      " (meta fee " + std::to_string(metaFee.value()) +
                                                      " + newAccountFee " + std::to_string(flat) +
                                                      "): " + std::to_string(tx.fee));
  }
  return {};
}

} // namespace

chain_tx::Roe<size_t>
NewUserTxHandler::getBillableCustomMetaSizeForFee(
    const BlockChainConfig &config, const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxNewUser>(
      tx, "getBillableCustomMetaSizeForFee", "TxNewUser");
  if (!pRoe) {
    return pRoe.error();
  }
  return chain_tx::billableUserCustomMetaSize(config, pRoe.value()->meta);
}

chain_tx::Roe<uint64_t>
NewUserTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                     uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  auto pRoe = chain_tx::expectTx<Ledger::TxNewUser>(tx, "getSignerAccountId",
                                                    "TxNewUser");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->fromWalletId;
}

chain_tx::Roe<bool>
NewUserTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                        uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxNewUser>(tx, "matchesWalletForIndex",
                                                    "TxNewUser");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  return p->fromWalletId == walletId || p->toWalletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
NewUserTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxNewUser>(tx, "getIdempotencyKey", "TxNewUser");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(
      std::make_pair(p->fromWalletId, p->idempotentId));
}

chain_tx::Roe<void> NewUserTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                 AccountBuffer &bank,
                                                 const BlockApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxNewUser>(tx, "applyBlock", "TxNewUser");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  // Genesis bootstrap (system and genesis miner accounts) runs as block 0
  // with idempotentId == 0 by construction; every other new-user tx must
  // carry a non-zero id so it can't be replayed onto a later block.
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, p->idempotentId, p->fromWalletId, p->validationTsMin,
          p->validationTsMax, c.blockSlot, c.admissionMode,
          /*requireNonZeroId=*/c.blockId != 0);
      !idem) {
    return idem;
  }
  return applyNewUser(*p, c.ctx, bank, c.blockId, false, c.admissionMode);
}

chain_tx::Roe<void> NewUserTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                  AccountBuffer &bank,
                                                  const BufferApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxNewUser>(tx, "applyBuffer", "TxNewUser");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, p->idempotentId, p->fromWalletId, p->validationTsMin,
          p->validationTsMax, c.effectiveSlot, c.admissionMode,
          /*requireNonZeroId=*/c.blockId != 0);
      !idem) {
    return idem;
  }
  if (auto seeded =
          chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, p->fee);
      !seeded) {
    return seeded;
  }
  if (auto seeded =
          chain_tx::seedCommittedAccount(bank, c.ctx.bank, p->fromWalletId);
      !seeded) {
    return seeded;
  }
  return applyNewUser(*p, c.ctx, bank, c.blockId, true,
                      chain_block::BlockAdmissionMode::Full);
}

chain_tx::Roe<void> NewUserTxHandler::applyNewUser(
    const Ledger::TxNewUser &tx, const TxContext &ctx,
    AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
    chain_block::BlockAdmissionMode admissionMode) const {
  // Id range first: structural, before fee and balance checks.
  if (AccountIds::isSystemAccount(tx.toWalletId) && blockId != 0) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "System account ids (< " + std::to_string(AccountBuffer::ID_FIRST_ISSUED) +
            ") are created only in the genesis block");
  }

  if (chain_block::admissionTxStrict(admissionMode)) {
    if (auto feeGate = requireNewAccountFee(tx, ctx, blockId); !feeGate) {
      return feeGate;
    }
  }

  const bool toWalletExists =
      bank.hasAccount(tx.toWalletId) ||
      (isBufferMode && ctx.bank.hasAccount(tx.toWalletId));
  if (toWalletExists) {
    return chain_tx::TxError(
        chain_err::E_ACCOUNT_EXISTS,
        "Account already exists: " + std::to_string(tx.toWalletId));
  }

  auto spendingResult = bank.verifySpendingPower(
      tx.fromWalletId, AccountBuffer::ID_GENESIS, tx.amount, tx.fee);
  if (!spendingResult) {
    return chain_tx::TxError(
        chain_err::E_ACCOUNT_BALANCE,
        "Source account must have sufficient balance: " +
            spendingResult.error().message);
  }

  // After genesis, genesis creates no accounts (each would mint its balance
  // and fee); issued ids come only from the registrar, user ids from anyone.
  if (blockId != 0) {
    if (tx.fromWalletId == AccountBuffer::ID_GENESIS) {
      return chain_tx::TxError(
          chain_err::E_TX_VALIDATION,
          "Genesis creates no accounts after the genesis block");
    }
    if (tx.toWalletId < AccountBuffer::ID_FIRST_USER &&
        tx.fromWalletId != AccountBuffer::ID_REGISTRAR) {
      return chain_tx::TxError(
          chain_err::E_TX_VALIDATION,
          "Issued account ids (< " + std::to_string(AccountBuffer::ID_FIRST_USER) +
              ") are created only by the registrar");
    }
  }

  auto userAccountRoe = chain_tx::loadAndValidateUserAccountMeta(
      tx.meta, ctx.crypto, bank, isBufferMode ? &ctx.bank : nullptr,
      tx.toWalletId, "Failed to deserialize user account: " + tx.meta);
  if (!userAccountRoe) {
    return userAccountRoe.error();
  }
  Client::UserAccount userAccount = std::move(userAccountRoe.value());

  if (userAccount.wallet.mBalances.size() != 1) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "User account must have exactly one balance");
  }
  auto it = userAccount.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
  if (it == userAccount.wallet.mBalances.end()) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User account must have balance in ID_GENESIS token");
  }
  if (it->second != static_cast<int64_t>(tx.amount)) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User account must have balance in ID_GENESIS token: " +
            std::to_string(it->second));
  }

  AccountBuffer::Account account;
  account.id = tx.toWalletId;
  account.blockId = blockId;
  account.wallet = userAccount.wallet;
  account.wallet.mBalances.clear();

  auto addResult = bank.add(account);
  if (!addResult) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_BUFFER,
        "Failed to add user account to buffer: " + addResult.error().message);
  }

  auto transferResult =
      bank.transferBalance(tx.fromWalletId, tx.toWalletId,
                           AccountBuffer::ID_GENESIS, tx.amount, tx.fee);
  if (!transferResult) {
    return chain_tx::TxError(
        chain_err::E_TX_TRANSFER,
        "Failed to transfer balance: " + transferResult.error().message);
  }

  return {};
}

std::optional<std::string>
NewUserTxHandler::getUserAccountMetaForTx(const Ledger::TypedTx &tx,
                                          uint64_t accountId) const {
  const auto *p = std::get_if<Ledger::TxNewUser>(&tx);
  if (!p) {
    return std::nullopt;
  }
  if (accountId == AccountBuffer::ID_GENESIS || p->toWalletId != accountId) {
    return std::nullopt;
  }
  return p->meta;
}

} // namespace pp
