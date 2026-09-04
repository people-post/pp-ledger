#include "RenewalTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxTyped.h"
#include "Types.h"
#include "../client/Client.h"

#include <variant>

namespace pp {

chain_tx::Roe<size_t>
RenewalTxHandler::getBillableCustomMetaSizeForFee(const BlockChainConfig &config,
                                                  const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxRenewal>(
      tx, "getBillableCustomMetaSizeForFee", "TxRenewal");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (p->walletId == AccountBuffer::ID_GENESIS) {
    if (p->meta.size() <= config.freeCustomMetaSize) {
      return 0;
    }
    GenesisAccountMeta gm;
    if (!gm.ltsFromString(p->meta)) {
      return chain_tx::TxError(
          chain_err::E_INTERNAL_DESERIALIZE,
          "Failed to deserialize genesis metadata for fee calculation");
    }
    return gm.genesis.meta.size();
  }
  return chain_tx::billableUserCustomMetaSize(config, p->meta);
}

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
  auto pRoe = chain_tx::expectTx<Ledger::TxRenewal>(tx, "getSignerAccountId",
                                                    "TxRenewal");
  if (!pRoe) {
    return pRoe.error();
  }
  return slotLeaderId != 0 ? slotLeaderId : pRoe.value()->walletId;
}

chain_tx::Roe<bool>
RenewalTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                        uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxRenewal>(tx, "matchesWalletForIndex",
                                                    "TxRenewal");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->walletId == walletId;
}

chain_tx::Roe<std::optional<uint64_t>>
RenewalTxHandler::getRenewalAccountIdIfAny(const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxRenewal>(
      tx, "getRenewalAccountIdIfAny", "TxRenewal");
  if (!pRoe) {
    return pRoe.error();
  }
  return std::optional<uint64_t>(pRoe.value()->walletId);
}

chain_tx::Roe<void> RenewalTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                  AccountBuffer &bank,
                                                  const BufferApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxRenewal>(tx, "applyBuffer", "TxRenewal");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (p->walletId == AccountBuffer::ID_GENESIS) {
    if (auto seeded = chain_tx::seedCommittedAccount(
            bank, c.ctx.bank, AccountBuffer::ID_GENESIS);
        !seeded) {
      return seeded;
    }
    if (auto seeded =
            chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, p->fee);
        !seeded) {
      return seeded;
    }
    return applyRenewal(*p, c.ctx, bank, c.blockId, true, true);
  }
  const auto userUpsert = renewalToUserUpsert(*p);
  return applyUserUpdateBufferCommon(userUpsert, bank, c);
}

chain_tx::Roe<void> RenewalTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                 AccountBuffer &bank,
                                                 const BlockApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxRenewal>(tx, "applyBlock", "TxRenewal");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
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

  if (auto shape = chain_tx::validateGenesisWalletShape(gm.genesis.wallet);
      !shape) {
    return shape;
  }

  if (isStrictMode) {
    if (auto feeGate = chain_tx::requireMinimumFee(
            ctx.optChainConfig, ctx.fnBillableCustomMetaSizeForFee,
            Ledger::TypedTx(tx), tx.fee,
            "Chain config required for strict genesis renewal fee validation",
            "Genesis renewal fee below minimum: ");
        !feeGate) {
      return feeGate;
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

  if (auto replaced = chain_tx::replaceGenesisAccount(bank, blockId,
                                                      gm.genesis.wallet);
      !replaced) {
    return replaced;
  }

  return chain_tx::creditFeeToFeeAccount(
      bank, tx.fee, "Failed to credit fee to fee account: ");
}

std::optional<std::string>
RenewalTxHandler::getUserAccountMetaForTx(const Ledger::TypedTx &tx,
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
RenewalTxHandler::getGenesisAccountMetaForTx(const Ledger::TypedTx &tx,
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
