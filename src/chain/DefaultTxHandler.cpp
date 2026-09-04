#include "DefaultTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxTyped.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t>
DefaultTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                     uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  auto pRoe = chain_tx::expectTx<Ledger::TxDefault>(tx, "getSignerAccountId",
                                                    "TxDefault");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->fromWalletId;
}

chain_tx::Roe<bool>
DefaultTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                        uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxDefault>(tx, "matchesWalletForIndex",
                                                    "TxDefault");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  return p->fromWalletId == walletId || p->toWalletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
DefaultTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxDefault>(tx, "getIdempotencyKey", "TxDefault");
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

chain_tx::Roe<void> DefaultTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                AccountBuffer &bank,
                                                const BufferApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxDefault>(tx, "applyBuffer", "TxDefault");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, p->idempotentId, p->fromWalletId, p->validationTsMin,
          p->validationTsMax, c.effectiveSlot, c.isStrictMode);
      !idem) {
    return idem;
  }
  if (auto seeded =
          chain_tx::seedCommittedAccount(bank, c.ctx.bank, p->fromWalletId);
      !seeded) {
    return seeded;
  }
  if (auto seeded =
          chain_tx::seedCommittedAccount(bank, c.ctx.bank, p->toWalletId);
      !seeded) {
    return seeded;
  }
  if (auto seeded =
          chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, p->fee);
      !seeded) {
    return seeded;
  }
  return applyDefaultTransferStrict(*p, c.ctx, bank);
}

chain_tx::Roe<void> DefaultTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                AccountBuffer &bank,
                                                const BlockApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxDefault>(tx, "applyBlock", "TxDefault");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, p->idempotentId, p->fromWalletId, p->validationTsMin,
          p->validationTsMax, c.blockSlot, c.isStrictMode);
      !idem) {
    return idem;
  }
  if (c.isStrictMode) {
    return applyDefaultTransferStrict(*p, c.ctx, bank);
  }
  return applyDefaultTransferLoose(*p, c.ctx, bank);
}

chain_tx::Roe<void> DefaultTxHandler::applyDefaultTransferStrict(
    const Ledger::TxDefault &tx, const TxContext &ctx,
    AccountBuffer &bank) const {
  if (auto feeGate = chain_tx::requireMinimumFee(
          ctx.optChainConfig, ctx.fnBillableCustomMetaSizeForFee,
          Ledger::TypedTx(tx), tx.fee,
          "Chain config required for strict default transfer fee validation",
          "Transaction fee below minimum: ");
      !feeGate) {
    return feeGate;
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
    AccountBuffer &bank) const {
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
      if (auto credited = chain_tx::creditFeeToFeeAccount(
              bank, tx.fee, "Failed to credit fee: ");
          !credited) {
        return credited;
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
