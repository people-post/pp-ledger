#ifndef PP_LEDGER_TX_FEES_H
#define PP_LEDGER_TX_FEES_H

#include "TxError.h"
#include "Types.h"
#include "AccountBuffer.h"
#include "../ledger/Ledger.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pp::chain_tx {

struct TxView {
  uint64_t tokenId{0};
  uint64_t amount{0};
  uint64_t fee{0};
  std::string_view meta{};
  uint64_t idempotentId{0};
  int64_t validationTsMin{0};
  int64_t validationTsMax{0};
  uint64_t fromWalletId{0};
  uint64_t toWalletId{0};
};

template <typename TxT>
TxView makeTxView(const TxT &tx) {
  return TxView{
      .tokenId = tx.tokenId,
      .amount = tx.amount,
      .fee = tx.fee,
      .meta = tx.meta,
      .idempotentId = tx.idempotentId,
      .validationTsMin = tx.validationTsMin,
      .validationTsMax = tx.validationTsMax,
      .fromWalletId = tx.fromWalletId,
      .toWalletId = tx.toWalletId,
  };
}

Roe<uint64_t> calculateMinimumFeeFromNonFreeMetaSize(
    const BlockChainConfig &config, uint64_t nonFreeCustomMetaSizeBytes);

Roe<size_t> extractNonFreeCustomMetaSizeForFee(const BlockChainConfig &config,
                                               uint16_t type,
                                               const TxView &tx);

template <typename TxT>
Roe<size_t> extractNonFreeCustomMetaSizeForFee(const BlockChainConfig &config,
                                               uint16_t type,
                                               const TxT &tx) {
  return extractNonFreeCustomMetaSizeForFee(config, type, makeTxView(tx));
}

Roe<uint64_t> calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                                uint16_t type,
                                                const TxView &tx);

template <typename TxT>
Roe<uint64_t> calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                                uint16_t type, const TxT &tx) {
  return calculateMinimumFeeForTransaction(config, type, makeTxView(tx));
}

/** Minimum renewal fee from serialized account meta at the account's block. */
Roe<uint64_t> calculateMinimumFeeForAccountMeta(
    const Ledger &ledger, const BlockChainConfig &config,
    const AccountBuffer &bank, uint64_t accountId);

} // namespace pp::chain_tx

#endif
