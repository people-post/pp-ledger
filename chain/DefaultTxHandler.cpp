#include "DefaultTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"

namespace pp {

chain_tx::Roe<void> DefaultTxHandler::applyDefaultTransferStrict(
    const Ledger::TxDefault &tx, const TxContext &ctx,
    AccountBuffer &bank) {
  if (!ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Chain config required for strict default transfer fee validation");
  }
  auto minimumFeeResult = chain_tx::calculateMinimumFeeForTransaction(
      ctx.optChainConfig.value(), Ledger::T_DEFAULT, tx);
  if (!minimumFeeResult) {
    return minimumFeeResult.error();
  }
  const uint64_t minFeePerTransaction = minimumFeeResult.value();
  if (tx.fee < minFeePerTransaction) {
    return chain_tx::TxError(
        chain_err::E_TX_FEE,
        "Transaction fee below minimum: " + std::to_string(tx.fee));
  }

  auto transferResult = bank.transferBalance(tx.fromWalletId, tx.toWalletId,
                                             tx.tokenId, tx.amount, tx.fee);
  if (!transferResult) {
    return chain_tx::TxError(
        chain_err::E_TX_TRANSFER,
        "Transaction failed: " + transferResult.error().message);
  }
  return {};
}

chain_tx::Roe<void> DefaultTxHandler::applyDefaultTransferLoose(
    const Ledger::TxDefault &tx, [[maybe_unused]] const TxContext &ctx,
    AccountBuffer &bank) {
  if (bank.hasAccount(tx.fromWalletId)) {
    if (bank.hasAccount(tx.toWalletId)) {
      auto transferResult = bank.transferBalance(
          tx.fromWalletId, tx.toWalletId, tx.tokenId, tx.amount, tx.fee);
      if (!transferResult) {
        return chain_tx::TxError(
            chain_err::E_TX_TRANSFER,
            "Failed to transfer balance: " + transferResult.error().message);
      }
    } else {
      if (tx.tokenId == AccountBuffer::ID_GENESIS) {
        auto withdrawResult = bank.withdrawBalance(
            tx.fromWalletId, tx.tokenId,
            static_cast<int64_t>(tx.amount) + static_cast<int64_t>(tx.fee));
        if (!withdrawResult) {
          return chain_tx::TxError(
              chain_err::E_TX_TRANSFER,
              "Failed to withdraw balance: " + withdrawResult.error().message);
        }
      } else {
        auto withdrawAmountResult = bank.withdrawBalance(
            tx.fromWalletId, tx.tokenId, static_cast<int64_t>(tx.amount));
        if (!withdrawAmountResult) {
          return chain_tx::TxError(
              chain_err::E_TX_TRANSFER,
              "Failed to withdraw balance: " +
                  withdrawAmountResult.error().message);
        }
        if (tx.fee > 0) {
          auto withdrawFeeResult =
              bank.withdrawBalance(tx.fromWalletId, AccountBuffer::ID_GENESIS,
                                   static_cast<int64_t>(tx.fee));
          if (!withdrawFeeResult) {
            return chain_tx::TxError(
                chain_err::E_TX_TRANSFER,
                "Failed to withdraw fee: " + withdrawFeeResult.error().message);
          }
        }
      }
      if (tx.fee > 0 && bank.hasAccount(AccountBuffer::ID_FEE)) {
        auto depositFeeResult = bank.depositBalance(
            AccountBuffer::ID_FEE, AccountBuffer::ID_GENESIS,
            static_cast<int64_t>(tx.fee));
        if (!depositFeeResult) {
          return chain_tx::TxError(
              chain_err::E_TX_TRANSFER,
              "Failed to credit fee: " + depositFeeResult.error().message);
        }
      }
    }
  } else {
    if (bank.hasAccount(tx.toWalletId)) {
      auto depositResult = bank.depositBalance(
          tx.toWalletId, tx.tokenId, static_cast<int64_t>(tx.amount));
      if (!depositResult) {
        return chain_tx::TxError(
            chain_err::E_TX_TRANSFER,
            "Failed to deposit balance: " + depositResult.error().message);
      }
    }
  }

  return {};
}

} // namespace pp
