#include "TxLedgerMeta.h"
#include "ErrorCodes.h"
#include "ledger/Ledger.h"

#include <limits>
#include <optional>

namespace pp::chain_tx {

namespace {
template <class... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

std::optional<std::string> userAccountMetaForRecord(const Ledger::Record &rec,
                                                    uint64_t accountId) {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return std::nullopt;
  }
  const Ledger::TypedTx &typed = typedRoe.value();
  return std::visit(
      Overloaded{
          [&](const Ledger::TxNewUser &tx) -> std::optional<std::string> {
            if (accountId == AccountBuffer::ID_GENESIS ||
                tx.toWalletId != accountId) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const Ledger::TxUserUpdate &tx) -> std::optional<std::string> {
            if (accountId == AccountBuffer::ID_GENESIS ||
                tx.walletId != accountId) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const Ledger::TxRenewal &tx) -> std::optional<std::string> {
            if (accountId == AccountBuffer::ID_GENESIS ||
                tx.walletId != accountId) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const auto &) -> std::optional<std::string> {
            return std::nullopt;
          },
      },
      typed);
}

std::optional<std::string> genesisAccountMetaForRecord(
    const Ledger::Record &rec, const Ledger::Block &block) {
  auto typedRoe = Ledger::decodeRecord(rec);
  if (!typedRoe) {
    return std::nullopt;
  }
  const Ledger::TypedTx &typed = typedRoe.value();
  return std::visit(
      Overloaded{
          [&](const Ledger::TxGenesis &tx) -> std::optional<std::string> {
            if (block.index != 0) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const Ledger::TxConfig &tx) -> std::optional<std::string> {
            return tx.meta;
          },
          [&](const Ledger::TxRenewal &tx) -> std::optional<std::string> {
            if (tx.walletId != AccountBuffer::ID_GENESIS) {
              return std::nullopt;
            }
            return tx.meta;
          },
          [&](const auto &) -> std::optional<std::string> {
            return std::nullopt;
          },
      },
      typed);
}
} // namespace

Roe<Client::UserAccount>
getUserAccountMetaFromBlock(const Ledger::Block &block, uint64_t accountId) {
  for (auto it = block.records.rbegin(); it != block.records.rend();
       ++it) {
    auto metaOpt = userAccountMetaForRecord(*it, accountId);
    if (!metaOpt) {
      continue;
    }
    const std::string &meta = *metaOpt;
    Client::UserAccount userAccount;
    if (!userAccount.ltsFromString(meta)) {
      return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                     "Failed to deserialize account info: " +
                         std::to_string(meta.size()) + " bytes");
    }
    return userAccount;
  }

  return TxError(chain_err::E_INTERNAL,
                 "No prior user/renewal from this account in block");
}

Roe<GenesisAccountMeta>
getGenesisAccountMetaFromBlock(const Ledger::Block &block) {
  for (auto it = block.records.rbegin(); it != block.records.rend();
       ++it) {
    auto metaOpt = genesisAccountMetaForRecord(*it, block);
    if (!metaOpt) {
      continue;
    }
    const std::string &meta = *metaOpt;

    GenesisAccountMeta gm;
    if (!gm.ltsFromString(meta)) {
      return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                     "Failed to deserialize checkpoint: " +
                         std::to_string(meta.size()) + " bytes");
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
