#ifndef PP_LEDGER_TX_FEES_H
#define PP_LEDGER_TX_FEES_H

#include "TxError.h"
#include "Types.h"
#include "AccountBuffer.h"
#include "../ledger/Ledger.h"

#include <cstddef>
#include <cstdint>

namespace pp::chain_tx {

Roe<uint64_t> calculateMinimumFeeFromNonFreeMetaSize(
    const BlockChainConfig &config, uint64_t nonFreeCustomMetaSizeBytes);

Roe<size_t> extractNonFreeCustomMetaSizeForFee(const BlockChainConfig &config,
                                               const Ledger::Transaction &tx);

Roe<uint64_t> calculateMinimumFeeForTransaction(const BlockChainConfig &config,
                                                const Ledger::Transaction &tx);

/** Minimum renewal fee from serialized account meta at the account's block. */
Roe<uint64_t> calculateMinimumFeeForAccountMeta(
    const Ledger &ledger, const BlockChainConfig &config,
    const AccountBuffer &bank, uint64_t accountId);

} // namespace pp::chain_tx

#endif
