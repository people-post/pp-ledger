#include "NewUserTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "../client/Client.h"

#include <string>

namespace pp {

chain_tx::Roe<void> NewUserTxHandler::applyNewUser(
    const Ledger::TxNewUser &tx, const TxContext &ctx,
    AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
    bool isStrictMode) {
  if (isStrictMode) {
    if (!ctx.optChainConfig.has_value()) {
      return chain_tx::TxError(
          chain_err::E_INTERNAL,
          "Chain config required for strict new-user fee validation");
    }
    const pp::TypedTx typedTx(tx);
    auto minimumFeeResult = chain_tx::calculateMinimumFeeForTransaction(
        ctx.optChainConfig.value(), typedTx);
    if (!minimumFeeResult) {
      return minimumFeeResult.error();
    }
    const uint64_t minFeePerTransaction = minimumFeeResult.value();
    if (tx.fee < minFeePerTransaction) {
      return chain_tx::TxError(chain_err::E_TX_FEE,
                               "New user transaction fee below minimum: " +
                                   std::to_string(tx.fee));
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

  if (tx.fromWalletId != AccountBuffer::ID_GENESIS &&
      tx.toWalletId < AccountBuffer::ID_FIRST_USER) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "New user account id must be larger than: " +
            std::to_string(AccountBuffer::ID_FIRST_USER));
  }

  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE,
                             "Failed to deserialize user account: " + tx.meta);
  }

  if (!ctx.crypto.isSupported(userAccount.wallet.keyType)) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "Unsupported key type: " +
            std::to_string(int(userAccount.wallet.keyType)));
  }
  if (userAccount.wallet.publicKeys.empty()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "User account must have at least one public key");
  }
  if (userAccount.wallet.minSignatures < 1) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User account must require at least one signature");
  }
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
  if (it->second != tx.amount) {
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

} // namespace pp
