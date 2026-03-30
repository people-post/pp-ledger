#include "GenesisRenewalTxHandler.h"
#include "RenewalUtil.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "Types.h"
#include "../ledger/TypedTx.h"

#include <variant>

namespace pp {

chain_tx::Roe<void> GenesisRenewalTxHandler::applyBuffer(const TypedTx &tx,
                                                         AccountBuffer &bank,
                                                         const BufferApplyContext &c) {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer: expected TxRenewal");
  }
  if (p->walletId == AccountBuffer::ID_GENESIS) {
    if (auto r = c.host.seedAccountIntoBuffer(bank, AccountBuffer::ID_GENESIS);
        !r) {
      return r;
    }
    if (p->fee > 0) {
      if (auto r = c.host.seedAccountIntoBuffer(bank, AccountBuffer::ID_FEE);
          !r) {
        return r;
      }
    }
    return applyGenesisRenewal(*p, c.ctx, bank, c.blockId, true, true);
  }
  if (c.userUpdateHandler == nullptr) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                           "applyBuffer: user-update handler required");
  }
  TypedTx asUserUpdate = renewalToUserUpsert(*p);
  return c.userUpdateHandler->applyBuffer(asUserUpdate, bank, c);
}

chain_tx::Roe<void> GenesisRenewalTxHandler::applyBlock(const TypedTx &tx,
                                                       AccountBuffer &bank,
                                                       const BlockApplyContext &c) {
  const auto *p = std::get_if<Ledger::TxRenewal>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: expected TxRenewal");
  }
  if (p->walletId == AccountBuffer::ID_GENESIS) {
    return applyGenesisRenewal(*p, c.ctx, bank, c.blockId, false,
                               c.isStrictMode);
  }
  if (c.userUpdateHandler == nullptr) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: user-update handler required");
  }
  TypedTx asUserUpdate = renewalToUserUpsert(*p);
  return c.userUpdateHandler->applyBlock(asUserUpdate, bank, c);
}

chain_tx::Roe<void> GenesisRenewalTxHandler::applyGenesisRenewal(
    const Ledger::TxRenewal &tx, const TxContext &ctx,
    AccountBuffer &bank, uint64_t blockId, [[maybe_unused]] bool isBufferMode,
    bool isStrictMode) {
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
    const pp::TypedTx typedTx(tx);
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

} // namespace pp
