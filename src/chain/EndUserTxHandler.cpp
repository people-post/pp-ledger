#include "EndUserTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxTyped.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t>
EndUserTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                     uint64_t slotLeaderId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxEndUser>(tx, "getSignerAccountId",
                                                    "TxEndUser");
  if (!pRoe) {
    return pRoe.error();
  }
  return slotLeaderId != 0 ? slotLeaderId : pRoe.value()->walletId;
}

chain_tx::Roe<bool>
EndUserTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                        uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxEndUser>(tx, "matchesWalletForIndex",
                                                    "TxEndUser");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->walletId == walletId;
}

chain_tx::Roe<std::optional<uint64_t>>
EndUserTxHandler::getRenewalAccountIdIfAny(const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxEndUser>(
      tx, "getRenewalAccountIdIfAny", "TxEndUser");
  if (!pRoe) {
    return pRoe.error();
  }
  return std::optional<uint64_t>(pRoe.value()->walletId);
}

chain_tx::Roe<void> EndUserTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                  AccountBuffer &bank,
                                                  const BufferApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxEndUser>(tx, "applyBuffer", "TxEndUser");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (auto seeded =
          chain_tx::seedCommittedAccount(bank, c.ctx.bank, p->walletId);
      !seeded) {
    return seeded;
  }
  if (auto seeded = chain_tx::seedCommittedAccount(
          bank, c.ctx.bank, AccountBuffer::ID_RECYCLE);
      !seeded) {
    return seeded;
  }
  return applyEndUser(*p, c.ctx, bank, true);
}

chain_tx::Roe<void> EndUserTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                 AccountBuffer &bank,
                                                 const BlockApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxEndUser>(tx, "applyBlock", "TxEndUser");
  if (!pRoe) {
    return pRoe.error();
  }
  return applyEndUser(*pRoe.value(), c.ctx, bank, false);
}

chain_tx::Roe<void> EndUserTxHandler::applyEndUser(
    const Ledger::TxEndUser &tx, const TxContext &ctx,
    AccountBuffer &bank, [[maybe_unused]] bool isBufferMode) const {

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

  if (!ctx.fnAccountMetaForRecord.has_value()) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Account meta extractors not configured on TxContext");
  }
  const auto &metaFns = *ctx.fnAccountMetaForRecord;
  auto minimumFeeResult = chain_tx::calculateMinimumFeeForAccountMeta(
      ctx.ledger, ctx.optChainConfig.value(), bank, tx.walletId,
      metaFns.fnUser, metaFns.fnGenesis);
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
