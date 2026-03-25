#ifndef PP_LEDGER_CHAIN_TX_FEES_H
#define PP_LEDGER_CHAIN_TX_FEES_H

#include "ChainTxError.h"
#include "ChainTypes.h"
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

} // namespace pp::chain_tx

#endif
