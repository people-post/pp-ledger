#ifndef PP_LEDGER_TX_FEES_H
#define PP_LEDGER_TX_FEES_H

#include "TxError.h"
#include "TxLedgerMeta.h"
#include "Types.h"
#include "AccountBuffer.h"
#include "../ledger/Ledger.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace pp::chain_tx {

Roe<uint64_t> calculateMinimumFeeFromNonFreeMetaSize(
    const BlockChainConfig &config, uint64_t nonFreeCustomMetaSizeBytes);

/**
 * Inject tx-dependent logic for fee meta billing.
 *
 * The function returns the *billable* (pre-free-tier) custom-meta size in bytes
 * for fee purposes. `TxFees` applies max bound and free tier.
 *
 * Returning 0 means "no fee meta to bill".
 */
using FnBillableCustomMetaSizeForFee =
    std::function<Roe<size_t>(const BlockChainConfig &, const Ledger::TypedTx &)>;

Roe<uint64_t> calculateMinimumFeeForTransaction(
    const BlockChainConfig &config, const Ledger::TypedTx &tx,
    const FnBillableCustomMetaSizeForFee &fnBillableCustomMetaSizeForFee);

/** Minimum renewal fee from serialized account meta at the account's block. */
Roe<uint64_t> calculateMinimumFeeForAccountMeta(
    const Ledger &ledger, const BlockChainConfig &config,
    const AccountBuffer &bank, uint64_t accountId,
    const FnUserAccountMetaForRecord &fnUserMetaForRecord,
    const FnGenesisAccountMetaForRecord &fnGenesisMetaForRecord);

} // namespace pp::chain_tx

#endif
