#include "ChainTxFees.h"
#include "ChainErrorCodes.h"
#include "AccountBuffer.h"
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

Roe<size_t> extractNonFreeCustomMetaSizeForFee(
    const BlockChainConfig &config, const Ledger::Transaction &tx) {
  auto toNonFree = [&](size_t customMetaSizeBytes) -> Roe<size_t> {
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
  };

  if (tx.meta.size() <= config.freeCustomMetaSize) {
    return 0;
  }

  switch (tx.type) {
  case Ledger::Transaction::T_NEW_USER:
  case Ledger::Transaction::T_USER: {
    Client::UserAccount userAccount;
    if (!userAccount.ltsFromString(tx.meta)) {
      return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                     "Failed to deserialize user account metadata for fee "
                     "calculation");
    }
    return toNonFree(userAccount.meta.size());
  }
  case Ledger::Transaction::T_RENEWAL: {
    if (tx.fromWalletId == AccountBuffer::ID_GENESIS &&
        tx.toWalletId == AccountBuffer::ID_GENESIS) {
      GenesisAccountMeta gm;
      if (!gm.ltsFromString(tx.meta)) {
        return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                       "Failed to deserialize genesis metadata for fee "
                       "calculation");
      }
      return toNonFree(gm.genesis.meta.size());
    }

    Client::UserAccount userAccount;
    if (!userAccount.ltsFromString(tx.meta)) {
      return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                     "Failed to deserialize renewal metadata for fee "
                     "calculation");
    }
    return toNonFree(userAccount.meta.size());
  }
  default:
    return toNonFree(tx.meta.size());
  }
}

Roe<uint64_t> calculateMinimumFeeForTransaction(
    const BlockChainConfig &config, const Ledger::Transaction &tx) {
  auto nonFreeMetaSizeResult = extractNonFreeCustomMetaSizeForFee(config, tx);
  if (!nonFreeMetaSizeResult) {
    return nonFreeMetaSizeResult.error();
  }
  return calculateMinimumFeeFromNonFreeMetaSize(
      config, nonFreeMetaSizeResult.value());
}

} // namespace pp::chain_tx
