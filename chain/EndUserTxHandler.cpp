#include "EndUserTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "../ledger/TypedTx.h"

#include <variant>

namespace pp {

chain_tx::Roe<void> EndUserTxHandler::applyBuffer(const TypedTx &tx,
                                                  AccountBuffer &bank,
                                                  const BufferApplyContext &c) {
  const auto *p = std::get_if<Ledger::TxEndUser>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer: expected TxEndUser");
  }
  if (auto r = c.host.seedAccountIntoBuffer(bank, p->walletId); !r) {
    return r;
  }
  if (auto r = c.host.seedAccountIntoBuffer(bank, AccountBuffer::ID_RECYCLE);
      !r) {
    return r;
  }
  return applyEndUser(*p, c.ctx, bank, true);
}

chain_tx::Roe<void> EndUserTxHandler::applyBlock(const TypedTx &tx,
                                                 AccountBuffer &bank,
                                                 const BlockApplyContext &c) {
  const auto *p = std::get_if<Ledger::TxEndUser>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: expected TxEndUser");
  }
  return applyEndUser(*p, c.ctx, bank, false);
}

chain_tx::Roe<void> EndUserTxHandler::applyEndUser(
    const Ledger::TxEndUser &tx, const TxContext &ctx,
    AccountBuffer &bank, [[maybe_unused]] bool isBufferMode) {
  (void)ctx;

  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "User end transaction must have fee 0");
  }

  if (!ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Chain config required for end-user minimum fee check");
  }

  if (!bank.hasAccount(tx.walletId)) {
    return chain_tx::TxError(
        chain_err::E_ACCOUNT_NOT_FOUND,
        "User account not found: " + std::to_string(tx.walletId));
  }

  auto minimumFeeResult = chain_tx::calculateMinimumFeeForAccountMeta(
      ctx.ledger, ctx.optChainConfig.value(), bank, tx.walletId);
  if (!minimumFeeResult) {
    return minimumFeeResult.error();
  }
  const uint64_t minFeePerTransaction = minimumFeeResult.value();
  if (bank.getBalance(tx.walletId, AccountBuffer::ID_GENESIS) >=
      static_cast<int64_t>(minFeePerTransaction)) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "User account must have less than " +
            std::to_string(minFeePerTransaction) +
            " balance in ID_GENESIS token");
  }

  auto writeOffResult = bank.writeOff(tx.walletId);
  if (!writeOffResult) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_BUFFER,
        "Failed to write off user account: " + writeOffResult.error().message);
  }

  return {};
}

} // namespace pp
