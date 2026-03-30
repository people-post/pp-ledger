#include "DefaultTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxIdempotency.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t>
DefaultTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                     uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  const auto *p = std::get_if<Ledger::TxDefault>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getSignerAccountId: expected TxDefault");
  }
  return p->fromWalletId;
}

chain_tx::Roe<bool>
DefaultTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                        uint64_t walletId) const {
  const auto *p = std::get_if<Ledger::TxDefault>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "matchesWalletForIndex: expected TxDefault");
  }
  return p->fromWalletId == walletId || p->toWalletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
DefaultTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  const auto *p = std::get_if<Ledger::TxDefault>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getIdempotencyKey: expected TxDefault");
  }
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(
      std::make_pair(p->fromWalletId, p->idempotentId));
}

chain_tx::Roe<void> DefaultTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                AccountBuffer &bank,
                                                const BufferApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxDefault>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer: expected TxDefault");
  }
  if (auto idem = chain_tx::validateIdempotencyRules(
          c.ctx.ledger, c.ctx.consensus, c.ctx.optChainConfig, p->idempotentId,
          p->fromWalletId, p->validationTsMin, p->validationTsMax, c.effectiveSlot,
          c.isStrictMode);
      !idem) {
    return idem;
  }
  if (auto r = bank.seedFromCommittedIfMissing(c.ctx.bank, p->fromWalletId);
      !r) {
    return chain_tx::TxError(r.error().code, r.error().message);
  }
  if (auto r = bank.seedFromCommittedIfMissing(c.ctx.bank, p->toWalletId); !r) {
    return chain_tx::TxError(r.error().code, r.error().message);
  }
  if (p->fee > 0) {
    if (auto r =
            bank.seedFromCommittedIfMissing(c.ctx.bank, AccountBuffer::ID_FEE);
        !r) {
      return chain_tx::TxError(r.error().code, r.error().message);
    }
  }
  return applyDefaultTransferStrict(*p, c.ctx, bank);
}

chain_tx::Roe<void> DefaultTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                AccountBuffer &bank,
                                                const BlockApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxDefault>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: expected TxDefault");
  }
  if (auto idem = chain_tx::validateIdempotencyRules(
          c.ctx.ledger, c.ctx.consensus, c.ctx.optChainConfig, p->idempotentId,
          p->fromWalletId, p->validationTsMin, p->validationTsMax, c.blockSlot,
          c.isStrictMode);
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
  if (!ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL,
        "Chain config required for strict default transfer fee validation");
  }
  const Ledger::TypedTx typedTx(tx);
  auto minimumFeeResult = chain_tx::calculateMinimumFeeForTransaction(
      ctx.optChainConfig.value(), typedTx);
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
