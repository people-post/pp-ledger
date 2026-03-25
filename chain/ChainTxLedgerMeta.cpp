#include "ChainTxLedgerMeta.h"
#include "ChainErrorCodes.h"

#include <limits>

namespace pp::chain_tx {

Roe<Client::UserAccount>
getUserAccountMetaFromBlock(const Ledger::Block &block, uint64_t accountId) {
  auto matchesUserAccount = [&](const Ledger::Transaction &tx) -> bool {
    switch (tx.type) {
    case Ledger::Transaction::T_NEW_USER:
      return accountId != AccountBuffer::ID_GENESIS &&
             tx.toWalletId == accountId;
    case Ledger::Transaction::T_USER:
      return accountId != AccountBuffer::ID_GENESIS &&
             tx.fromWalletId == accountId && tx.toWalletId == accountId;
    case Ledger::Transaction::T_RENEWAL:
      return tx.fromWalletId == accountId &&
             accountId != AccountBuffer::ID_GENESIS;
    default:
      return false;
    }
  };

  for (auto it = block.signedTxes.rbegin(); it != block.signedTxes.rend();
       ++it) {
    const auto &tx = it->obj;
    if (!matchesUserAccount(tx)) {
      continue;
    }
    Client::UserAccount userAccount;
    if (!userAccount.ltsFromString(tx.meta)) {
      return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                     "Failed to deserialize account info: " +
                         std::to_string(tx.meta.size()) + " bytes");
    }
    return userAccount;
  }

  return TxError(chain_err::E_INTERNAL,
                 "No prior user/renewal from this account in block");
}

Roe<GenesisAccountMeta>
getGenesisAccountMetaFromBlock(const Ledger::Block &block) {
  auto matchesAccount = [&](const Ledger::Transaction &tx) -> bool {
    switch (tx.type) {
    case Ledger::Transaction::T_GENESIS:
      return tx.fromWalletId == AccountBuffer::ID_GENESIS && block.index == 0;
    case Ledger::Transaction::T_CONFIG:
    case Ledger::Transaction::T_RENEWAL:
      return tx.fromWalletId == AccountBuffer::ID_GENESIS;
    default:
      return false;
    }
  };

  for (auto it = block.signedTxes.rbegin(); it != block.signedTxes.rend();
       ++it) {
    const auto &tx = it->obj;
    if (!matchesAccount(tx)) {
      continue;
    }

    GenesisAccountMeta gm;
    if (!gm.ltsFromString(tx.meta)) {
      return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                     "Failed to deserialize checkpoint: " +
                         std::to_string(tx.meta.size()) + " bytes");
    }
    return gm;
  }

  return TxError(chain_err::E_INTERNAL,
                 "No prior checkpoint/user/renewal from this account in block");
}

Roe<std::string> getUpdatedAccountMetadataForRenewal(
    const Ledger::Block &block, const AccountBuffer::Account &account,
    uint64_t minFee) {
  if (account.id == AccountBuffer::ID_GENESIS) {
    auto metaResult = getGenesisAccountMetaFromBlock(block);
    if (!metaResult) {
      return metaResult.error();
    }
    auto gm = metaResult.value();
    gm.genesis.wallet = account.wallet;

    auto it = gm.genesis.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
    if (it != gm.genesis.wallet.mBalances.end()) {
      const int64_t fee = static_cast<int64_t>(minFee);
      if (it->second < std::numeric_limits<int64_t>::min() + fee) {
        return TxError(chain_err::E_TX_VALIDATION,
                       "Genesis balance underflow while applying renewal fee");
      }
      it->second -= fee;
    }

    return gm.ltsToString();
  }

  auto userAccountRoe = getUserAccountMetaFromBlock(block, account.id);
  if (!userAccountRoe) {
    return userAccountRoe.error();
  }
  Client::UserAccount userAccount = std::move(userAccountRoe.value());
  userAccount.wallet = account.wallet;

  auto it = userAccount.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
  if (it != userAccount.wallet.mBalances.end()) {
    const int64_t fee = static_cast<int64_t>(minFee);
    if (account.id == AccountBuffer::ID_FEE) {
      if (it->second < std::numeric_limits<int64_t>::min() + fee) {
        return TxError(chain_err::E_TX_VALIDATION,
                       "Fee account balance underflow while applying renewal fee");
      }
    } else {
      if (it->second < fee) {
        return TxError(chain_err::E_ACCOUNT_BALANCE,
                       "Insufficient genesis balance for renewal fee");
      }
      it->second -= fee;
    }
  }
  return userAccount.ltsToString();
}

} // namespace pp::chain_tx
