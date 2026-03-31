#include "TxFees.h"
#include "ErrorCodes.h"
#include "AccountBuffer.h"
#include "TxLedgerMeta.h"
#include "../client/Client.h"

#include <limits>
#include <vector>

namespace pp::chain_tx {

namespace {

constexpr uint64_t BYTES_PER_KIB = 1024ULL;

uint64_t getFeeCoefficient(const std::vector<uint16_t> &coefficients,
                           size_t index) {
  return index < coefficients.size()
             ? static_cast<uint64_t>(coefficients[index])
             : 0ULL;
}

template <class... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

/** Fee uses "non-free" custom meta bytes: max bound, then free tier, then remainder. */
Roe<size_t> toNonFree(const BlockChainConfig &config,
                      size_t customMetaSizeBytes) {
  if (customMetaSizeBytes > config.maxCustomMetaSize) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Custom metadata exceeds maxCustomMetaSize: " +
                       std::to_string(customMetaSizeBytes) + " > " +
                       std::to_string(config.maxCustomMetaSize));
  }
  if (customMetaSizeBytes <= config.freeCustomMetaSize) {
    return 0;
  }
  return customMetaSizeBytes - config.freeCustomMetaSize;
}

/**
 * Serialized `Client::UserAccount` in tx.meta: if the whole blob is within the
 * free tier, fee meta is zero; else bill on embedded `userAccount.meta` size.
 */
Roe<size_t>
nonFreeFromSerializedUserAccountMeta(const BlockChainConfig &config,
                                     const std::string &serializedUserAccount) {
  if (serializedUserAccount.size() <= config.freeCustomMetaSize) {
    return 0;
  }
  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(serializedUserAccount)) {
    return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                   "Failed to deserialize user account metadata for fee "
                   "calculation");
  }
  return toNonFree(config, userAccount.meta.size());
}

Roe<size_t>
nonFreeFromSerializedGenesisAccountMeta(const BlockChainConfig &config,
                                        const std::string &serializedGm) {
  if (serializedGm.size() <= config.freeCustomMetaSize) {
    return 0;
  }
  GenesisAccountMeta gm;
  if (!gm.ltsFromString(serializedGm)) {
    return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                   "Failed to deserialize genesis metadata for fee calculation");
  }
  return toNonFree(config, gm.genesis.meta.size());
}

} // namespace

Roe<uint64_t> calculateMinimumFeeFromNonFreeMetaSize(
    const BlockChainConfig &config, uint64_t nonFreeCustomMetaSizeBytes) {
  if (config.minFeeCoefficients.empty()) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "minFeeCoefficients must not be empty");
  }
  const uint64_t nonFreeSizeKiB =
      nonFreeCustomMetaSizeBytes == 0
          ? 0ULL
          : (nonFreeCustomMetaSizeBytes + BYTES_PER_KIB - 1) / BYTES_PER_KIB;

  const unsigned __int128 a = getFeeCoefficient(config.minFeeCoefficients, 0);
  const unsigned __int128 b = getFeeCoefficient(config.minFeeCoefficients, 1);
  const unsigned __int128 c = getFeeCoefficient(config.minFeeCoefficients, 2);
  const unsigned __int128 x = nonFreeSizeKiB;
  const unsigned __int128 minimumFee = a + b * x + c * x * x;

  if (minimumFee >
      static_cast<unsigned __int128>(std::numeric_limits<int64_t>::max())) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Calculated minimum fee exceeds int64_t range");
  }

  return static_cast<uint64_t>(minimumFee);
}

Roe<size_t> extractNonFreeCustomMetaSizeForFee(const BlockChainConfig &config,
                                               const Ledger::TypedTx &tx) {
  return std::visit(
      Overloaded{
          [&](const Ledger::TxNewUser &t) -> Roe<size_t> {
            return nonFreeFromSerializedUserAccountMeta(config, t.meta);
          },
          [&](const Ledger::TxUserUpdate &t) -> Roe<size_t> {
            return nonFreeFromSerializedUserAccountMeta(config, t.meta);
          },
          [&](const Ledger::TxRenewal &t) -> Roe<size_t> {
            if (t.walletId == AccountBuffer::ID_GENESIS) {
              return nonFreeFromSerializedGenesisAccountMeta(config, t.meta);
            }
            return nonFreeFromSerializedUserAccountMeta(config, t.meta);
          },
          [&](const auto &t) -> Roe<size_t> {
            return toNonFree(config, t.meta.size());
          },
      },
      tx);
}

Roe<uint64_t> calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                                const Ledger::TypedTx &tx) {
  auto nonFreeMetaSizeResult = extractNonFreeCustomMetaSizeForFee(config, tx);
  if (!nonFreeMetaSizeResult) {
    return nonFreeMetaSizeResult.error();
  }
  return calculateMinimumFeeFromNonFreeMetaSize(
      config, nonFreeMetaSizeResult.value());
}

Roe<uint64_t> calculateMinimumFeeForAccountMeta(
    const Ledger &ledger, const BlockChainConfig &config,
    const AccountBuffer &bank, uint64_t accountId,
    const UserAccountMetaForRecordFn &userMetaForRecord,
    const GenesisAccountMetaForRecordFn &genesisMetaForRecord) {
  auto accountResult = bank.getAccount(accountId);
  if (!accountResult) {
    return TxError(chain_err::E_ACCOUNT_NOT_FOUND,
                   "User account not found: " + std::to_string(accountId));
  }

  auto blockResult = ledger.readBlock(accountResult.value().blockId);
  if (!blockResult) {
    return TxError(chain_err::E_BLOCK_NOT_FOUND,
                   "Block not found: " +
                       std::to_string(accountResult.value().blockId));
  }

  size_t metaSize = 0;

  if (accountId == AccountBuffer::ID_GENESIS) {
    auto metaResult = getGenesisAccountMetaFromBlock(
        blockResult.value().block, genesisMetaForRecord);
    if (!metaResult) {
      return metaResult.error();
    }
    metaSize = metaResult.value().genesis.meta.size();
  } else {
    auto userMetaResult = getUserAccountMetaFromBlock(
        blockResult.value().block, accountId, userMetaForRecord);
    if (!userMetaResult) {
      return userMetaResult.error();
    }
    metaSize = userMetaResult.value().meta.size();
  }

  if (metaSize > config.maxCustomMetaSize) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Custom metadata exceeds maxCustomMetaSize: " +
                       std::to_string(metaSize) + " > " +
                       std::to_string(config.maxCustomMetaSize));
  }

  const uint64_t nonFreeMetaSize =
      metaSize > config.freeCustomMetaSize
          ? static_cast<uint64_t>(metaSize) - config.freeCustomMetaSize
          : 0ULL;

  return calculateMinimumFeeFromNonFreeMetaSize(config, nonFreeMetaSize);
}

} // namespace pp::chain_tx
