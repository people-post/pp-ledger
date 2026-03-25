#include "UserTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "../client/Client.h"

#include <string>

namespace pp {

chain_tx::Roe<void> UserTxHandler::applyUserAccountUpsert(
    const Ledger::Transaction &tx, ChainTxContextConst &ctx,
    AccountBuffer &bank, uint64_t blockId, bool isBufferMode,
    bool isStrictMode) {
  (void)isBufferMode;

  if (tx.tokenId != AccountBuffer::ID_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User update transaction must use genesis token (ID_GENESIS)");
  }

  if (tx.fromWalletId != tx.toWalletId) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User update transaction must use same from and to wallet IDs");
  }

  if (isStrictMode) {
    if (!ctx.optChainConfig.has_value()) {
      return chain_tx::TxError(
          chain_err::E_INTERNAL,
          "Chain config required for strict user-update fee validation");
    }
    auto minimumFeeResult = chain_tx::calculateMinimumFeeForTransaction(
        ctx.optChainConfig.value(), tx);
    if (!minimumFeeResult) {
      return minimumFeeResult.error();
    }
    const uint64_t minFeePerTransaction = minimumFeeResult.value();
    if (tx.fee < minFeePerTransaction) {
      return chain_tx::TxError(
          chain_err::E_TX_FEE,
          "User update transaction fee below minimum: " +
              std::to_string(tx.fee));
    }
  }

  if (tx.amount != 0) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User update transaction must have amount 0");
  }

  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(tx.meta)) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_DESERIALIZE,
        "Failed to deserialize user meta for account " +
            std::to_string(tx.fromWalletId) + ": " +
            std::to_string(tx.meta.size()) + " bytes");
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

  auto bufferAccountResult = bank.getAccount(tx.fromWalletId);
  if (!bufferAccountResult) {
    if (isStrictMode) {
      return chain_tx::TxError(
          chain_err::E_ACCOUNT_NOT_FOUND,
          "User account not found in buffer: " +
              std::to_string(tx.fromWalletId));
    }
  } else {
    auto balanceVerifyResult = bank.verifyBalance(tx.fromWalletId, 0, tx.fee,
                                                    userAccount.wallet.mBalances);
    if (!balanceVerifyResult) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               balanceVerifyResult.error().message);
    }
  }

  bank.remove(tx.fromWalletId);

  AccountBuffer::Account account;
  account.id = tx.fromWalletId;
  account.blockId = blockId;
  account.wallet = userAccount.wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_BUFFER,
        "Failed to add user account to buffer: " + addResult.error().message);
  }

  if (tx.fee > 0 && bank.hasAccount(AccountBuffer::ID_FEE)) {
    auto depositResult = bank.depositBalance(
        AccountBuffer::ID_FEE, AccountBuffer::ID_GENESIS,
        static_cast<int64_t>(tx.fee));
    if (!depositResult) {
      return chain_tx::TxError(
          chain_err::E_TX_TRANSFER,
          "Failed to credit fee to fee account: " +
              depositResult.error().message);
    }
  }

  return {};
}

} // namespace pp
