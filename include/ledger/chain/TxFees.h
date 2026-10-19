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
#include <optional>
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

/**
 * Strict-mode gate shared by Default / NewUser / user-upsert / genesis renewal:
 * require chain config + fee-meta callback, compute minimum, compare `fee`.
 *
 * `feeBelowMinPrefix` is prepended to the numeric fee in the E_TX_FEE message.
 */
Roe<void> requireMinimumFee(
    const std::optional<BlockChainConfig> &optChainConfig,
    const std::optional<FnBillableCustomMetaSizeForFee>
        &fnBillableCustomMetaSizeForFee,
    const Ledger::TypedTx &typedTx, uint64_t fee,
    std::string_view configRequiredMsg, std::string_view feeBelowMinPrefix);

/** Minimum renewal fee from serialized account meta at the account's block. */
Roe<uint64_t> calculateMinimumFeeForAccountMeta(
    const Ledger &ledger, const BlockChainConfig &config,
    const AccountBuffer &bank, uint64_t accountId,
    const FnUserAccountMetaForRecord &fnUserMetaForRecord,
    const FnGenesisAccountMetaForRecord &fnGenesisMetaForRecord);

} // namespace pp::chain_tx

#endif
