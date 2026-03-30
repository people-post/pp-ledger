#include "GenesisRenewalTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "Types.h"

namespace pp {

chain_tx::Roe<void> GenesisRenewalTxHandler::applyGenesisRenewal(
    const Ledger::TxRenewal &tx, const TxContext &ctx,
    AccountBuffer &bank, uint64_t blockId, [[maybe_unused]] bool isBufferMode,
    bool isStrictMode) {
  if (tx.walletId != AccountBuffer::ID_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "Genesis renewal must use genesis wallet (ID_GENESIS -> ID_GENESIS)");
  }
  if (tx.tokenId != AccountBuffer::ID_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "Genesis renewal must use genesis token (ID_GENESIS)");
  }
  if (tx.amount != 0) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "Genesis renewal transaction must have amount 0");
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
    auto minimumFeeResult = chain_tx::calculateMinimumFeeForTransaction(
        ctx.optChainConfig.value(), Ledger::T_RENEWAL, tx);
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
