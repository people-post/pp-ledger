#include "RenewalTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "Types.h"

#include <variant>

namespace pp {

/** Map miner-signed user renewal payload to user-update upsert semantics. */
Ledger::TxUserUpdate renewalToUserUpsert(const Ledger::TxRenewal &tx) {
  Ledger::TxUserUpdate userTx;
  userTx.walletId = tx.walletId;
  userTx.fee = tx.fee;
  userTx.meta = tx.meta;
  userTx.idempotentId = 0;
  userTx.validationTsMin = 0;
  userTx.validationTsMax = 0;
  return userTx;
}

chain_tx::Roe<uint64_t>
RenewalTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                     uint64_t slotLeaderId) const {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getSignerAccountId: expected TxRenewal");
  }
  return slotLeaderId != 0 ? slotLeaderId : p->walletId;
}

chain_tx::Roe<bool>
RenewalTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                        uint64_t walletId) const {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "matchesWalletForIndex: expected TxRenewal");
  }
  return p->walletId == walletId;
}

chain_tx::Roe<std::optional<uint64_t>>
RenewalTxHandler::getRenewalAccountIdIfAny(const Ledger::TypedTx &tx) const {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getRenewalAccountIdIfAny: expected TxRenewal");
  }
  return std::optional<uint64_t>(p->walletId);
}

chain_tx::Roe<void> RenewalTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                  AccountBuffer &bank,
                                                  const BufferApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer: expected TxRenewal");
  }
  if (p->walletId == AccountBuffer::ID_GENESIS) {
    if (auto r =
            bank.seedFromCommittedIfMissing(c.ctx.bank, AccountBuffer::ID_GENESIS);
        !r) {
      return chain_tx::TxError(r.error().code, r.error().message);
    }
    if (p->fee > 0) {
      if (auto r =
              bank.seedFromCommittedIfMissing(c.ctx.bank, AccountBuffer::ID_FEE);
          !r) {
        return chain_tx::TxError(r.error().code, r.error().message);
      }
    }
    return applyRenewal(*p, c.ctx, bank, c.blockId, true, true);
  }
  const auto userUpsert = renewalToUserUpsert(*p);
  return applyUserUpdateBufferCommon(userUpsert, bank, c);
}

chain_tx::Roe<void> RenewalTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                 AccountBuffer &bank,
                                                 const BlockApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: expected TxRenewal");
  }
  if (p->walletId == AccountBuffer::ID_GENESIS) {
    return applyRenewal(*p, c.ctx, bank, c.blockId, false, c.isStrictMode);
  }
  const auto userUpsert = renewalToUserUpsert(*p);
  return applyUserUpdateBlockCommon(userUpsert, bank, c);
}

chain_tx::Roe<void> RenewalTxHandler::applyRenewal(
    const Ledger::TxRenewal &tx, const TxContext &ctx,
    AccountBuffer &bank, uint64_t blockId, [[maybe_unused]] bool isBufferMode,
    bool isStrictMode) const {
  if (tx.walletId != AccountBuffer::ID_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "Genesis renewal must use genesis wallet (ID_GENESIS -> ID_GENESIS)");
  }

  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.meta)) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_DESERIALIZE,
        "Failed to deserialize genesis renewal meta: " +
            std::to_string(tx.meta.size()) + " bytes");
  }

  if (gm.genesis.wallet.publicKeys.size() < 3) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis account must have at least 3 public keys");
  }
  if (gm.genesis.wallet.minSignatures < 2) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis account must have at least 2 signatures");
  }

  if (isStrictMode) {
    if (!ctx.optChainConfig.has_value()) {
      return chain_tx::TxError(
          chain_err::E_INTERNAL,
          "Chain config required for strict genesis renewal fee validation");
    }
    const Ledger::TypedTx typedTx(tx);
    auto minimumFeeResult = chain_tx::calculateMinimumFeeForTransaction(
        ctx.optChainConfig.value(), typedTx);
    if (!minimumFeeResult) {
      return minimumFeeResult.error();
    }
    const uint64_t minFeePerTransaction = minimumFeeResult.value();
    if (tx.fee < minFeePerTransaction) {
      return chain_tx::TxError(chain_err::E_TX_FEE,
                               "Genesis renewal fee below minimum: " +
                                   std::to_string(tx.fee));
    }
  }

  auto genesisAccountResult = bank.getAccount(AccountBuffer::ID_GENESIS);
  if (!genesisAccountResult) {
    if (isStrictMode) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND,
                               "Genesis account not found for renewal");
    }
    return {};
  }

  if (!bank.verifyBalance(AccountBuffer::ID_GENESIS, 0, tx.fee,
                          gm.genesis.wallet.mBalances)) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "Genesis account balance mismatch in renewal");
  }

  bank.remove(AccountBuffer::ID_GENESIS);

  AccountBuffer::Account account;
  account.id = AccountBuffer::ID_GENESIS;
  account.blockId = blockId;
  account.wallet = gm.genesis.wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_BUFFER,
        "Failed to add renewed genesis account: " + addResult.error().message);
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

std::optional<std::string>
RenewalTxHandler::userAccountMetaForTx(const Ledger::TypedTx &tx,
                                       uint64_t accountId) const {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return std::nullopt;
  }
  if (accountId == AccountBuffer::ID_GENESIS || p->walletId != accountId) {
    return std::nullopt;
  }
  return p->meta;
}

std::optional<std::string>
RenewalTxHandler::genesisAccountMetaForTx(const Ledger::TypedTx &tx,
                                          const Ledger::Block & /*block*/) const {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return std::nullopt;
  }
  if (p->walletId != AccountBuffer::ID_GENESIS) {
    return std::nullopt;
  }
  return p->meta;
}

} // namespace pp

