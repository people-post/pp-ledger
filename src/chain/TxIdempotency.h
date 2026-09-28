#ifndef PP_LEDGER_TX_IDEMPOTENCY_H
#define PP_LEDGER_TX_IDEMPOTENCY_H

#include "BlockAdmission.h"
#include "TxError.h"
#include "Types.h"
#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace pp::chain_tx {

/** Per-record (walletId, idempotentId) for cross-block idempotency scan. */
using FnIdempotencyKeyForRecord =
    std::function<Roe<std::optional<std::pair<uint64_t, uint64_t>>>(
        const Ledger::Record &)>;

Roe<void> checkIdempotency(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    uint64_t idempotentId, uint64_t fromWalletId, uint64_t slotMin,
    uint64_t slotMax, const FnIdempotencyKeyForRecord &fnIdempotencyKeyForRecord);

/**
 * `requireNonZeroId`: in strict (Full) admission, reject idempotentId == 0
 * instead of skipping the replay check for it. Fund-moving transaction types
 * (transfer / new-user / end-user / renewal) must pass true here -- an id of
 * 0 would otherwise bypass cross-block replay protection entirely. Types
 * with no fund movement (e.g. T_CONFIG) may keep the default.
 */
Roe<void> validateIdempotencyRules(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    uint64_t idempotentId, uint64_t fromWalletId, int64_t validationTsMin,
    int64_t validationTsMax, uint64_t effectiveSlot,
    chain_block::BlockAdmissionMode admissionMode,
    const FnIdempotencyKeyForRecord &fnIdempotencyKeyForRecord,
    bool requireNonZeroId = false);

} // namespace pp::chain_tx

#endif
