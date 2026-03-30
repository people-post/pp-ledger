#include "EndUserTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"

namespace pp {

chain_tx::Roe<void> EndUserTxHandler::applyEndUser(
    const Ledger::TxEndUser &tx, const TxContext &ctx,
    AccountBuffer &bank, [[maybe_unused]] bool isBufferMode) {
  (void)ctx;

  if (tx.tokenId != AccountBuffer::ID_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User end transaction must use genesis token (ID_GENESIS)");
  }

  if (tx.fromWalletId != tx.toWalletId) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User end transaction must use same from and to wallet IDs");
  }

  if (tx.amount != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "User end transaction must have amount 0");
  }

  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "User end transaction must have fee 0");
  }

  if (!ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Chain config required for end-user minimum fee check");
  }

  if (!bank.hasAccount(tx.fromWalletId)) {
    return chain_tx::TxError(
        chain_err::E_ACCOUNT_NOT_FOUND,
        "User account not found: " + std::to_string(tx.fromWalletId));
  }

  auto minimumFeeResult = chain_tx::calculateMinimumFeeForAccountMeta(
      ctx.ledger, ctx.optChainConfig.value(), bank, tx.fromWalletId);
  if (!minimumFeeResult) {
    return minimumFeeResult.error();
  }
  const uint64_t minFeePerTransaction = minimumFeeResult.value();
  if (bank.getBalance(tx.fromWalletId, AccountBuffer::ID_GENESIS) >=
      static_cast<int64_t>(minFeePerTransaction)) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User account must have less than " +
            std::to_string(minFeePerTransaction) +
            " balance in ID_GENESIS token");
  }

  auto writeOffResult = bank.writeOff(tx.fromWalletId);
  if (!writeOffResult) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_BUFFER,
        "Failed to write off user account: " + writeOffResult.error().message);
  }

  return {};
}

} // namespace pp
