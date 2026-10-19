#ifndef PP_LEDGER_TX_IDEMPOTENCY_H
#define PP_LEDGER_TX_IDEMPOTENCY_H

#include "ledger/chain/BlockAdmission.h"
#include "ledger/chain/TxError.h"
#include "ledger/chain/Types.h"
#include "ledger/consensus/SlotCommittee.h"
#include "ledger/ledger/Ledger.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace pp::chain_tx {

/** Per-record (walletId, idempotentId) for cross-block idempotency scan. */
using FnIdempotencyKeyForRecord =
    std::function<Roe<std::optional<std::pair<uint64_t, uint64_t>>>(
        const Ledger::Record &)>;

/**
 * What the idempotency scan needs of a block — its timestamp, slot and records' (wallet, idempotent
 * id) — kept so each block is read and decoded once rather than once per transaction checked
 * (load-resilience lr8: a full block re-read its window's blocks for every one of its transactions,
 * and full blocks made slot leaders miss their slots). Holds the newest kMaxBlocks blocks; starts
 * over when the ledger's blocks may have changed (another ledger, Ledger::getRevision). Thread-safe.
 */
class IdempotencyScanCache {
public:
  static constexpr size_t kMaxBlocks = 2048;

  struct Block {
    int64_t timestamp{0};
    uint64_t slot{0};
    /** (wallet, idempotent id) of the records that have one, in block order. */
    std::vector<std::pair<uint64_t, uint64_t>> keys;
    /** The first record whose key could not be worked out, after `keys` (the scan fails there). */
    std::optional<TxError> keyError;
  };

  /** Block `blockId` of `ledger`, read on a miss; null when it cannot be read. */
  std::shared_ptr<const Block> get(const Ledger &ledger, uint64_t blockId,
                                   const FnIdempotencyKeyForRecord &fnIdempotencyKeyForRecord);

private:
  std::mutex mu_;
  const Ledger *ledger_{nullptr};
  uint64_t revision_{0};
  std::map<uint64_t, std::shared_ptr<const Block>> blocks_;
};

/**
 * Whether a record of `fromWalletId` with `idempotentId` is in a block of slot slotMin..slotMax.
 * With `cache`, from what it keeps of each block (the same answer, errors included).
 */
Roe<void> checkIdempotency(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    uint64_t idempotentId, uint64_t fromWalletId, uint64_t slotMin,
    uint64_t slotMax, const FnIdempotencyKeyForRecord &fnIdempotencyKeyForRecord,
    IdempotencyScanCache *cache = nullptr);

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
    bool requireNonZeroId = false, IdempotencyScanCache *cache = nullptr);

} // namespace pp::chain_tx

#endif
