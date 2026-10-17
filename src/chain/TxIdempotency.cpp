#include "TxIdempotency.h"
#include "ErrorCodes.h"

namespace pp::chain_tx {

namespace {

Roe<void> duplicate(uint64_t idempotentId, uint64_t fromWalletId) {
  return TxError(chain_err::E_TX_IDEMPOTENCY, "Duplicate idempotent id: " + std::to_string(idempotentId) +
                                                  " for wallet: " + std::to_string(fromWalletId));
}

/** checkIdempotency from what `cache` keeps: Ledger::findBlockByTimestamp, then the scan, step for step. */
Roe<void> checkIdempotencyCached(const Ledger &ledger, const consensus::SlotCommittee &consensus,
                                 uint64_t idempotentId, uint64_t fromWalletId, uint64_t slotMin,
                                 uint64_t slotMax, const FnIdempotencyKeyForRecord &fnKey,
                                 IdempotencyScanCache &cache) {
  const int64_t tsStart = consensus.getSlotStartTime(slotMin);
  const uint64_t nextBlockId = ledger.getNextBlockId();
  uint64_t low = ledger.getStartingBlockId();
  if (low >= nextBlockId) {
    return {};
  }
  uint64_t high = nextBlockId;
  while (low < high) {
    const uint64_t mid = low + (high - low) / 2;
    const auto block = cache.get(ledger, mid, fnKey);
    if (!block) {
      return {}; // as when findBlockByTimestamp fails
    }
    if (block->timestamp >= tsStart) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }
  for (uint64_t blockId = low; blockId < nextBlockId; ++blockId) {
    const auto block = cache.get(ledger, blockId, fnKey);
    if (!block) {
      break;
    }
    if (block->slot > slotMax) {
      break;
    }
    if (block->slot < slotMin) {
      continue;
    }
    for (const auto &key : block->keys) {
      if (key.second == idempotentId && key.first == fromWalletId) {
        return duplicate(idempotentId, fromWalletId);
      }
    }
    if (block->keyError) {
      return *block->keyError;
    }
  }
  return {};
}

} // namespace

std::shared_ptr<const IdempotencyScanCache::Block>
IdempotencyScanCache::get(const Ledger &ledger, uint64_t blockId, const FnIdempotencyKeyForRecord &fnKey) {
  std::lock_guard<std::mutex> lock(mu_);
  if (ledger_ != &ledger || revision_ != ledger.getRevision()) {
    blocks_.clear();
    ledger_ = &ledger;
    revision_ = ledger.getRevision();
  }
  if (auto it = blocks_.find(blockId); it != blocks_.end()) {
    return it->second;
  }
  auto read = ledger.readBlock(blockId);
  if (!read) {
    return nullptr;
  }
  auto block = std::make_shared<Block>();
  block->timestamp = read.value().block.timestamp;
  block->slot = read.value().block.slot;
  for (const auto &record : read.value().block.records) {
    auto key = fnKey(record);
    if (!key) {
      block->keyError = key.error();
      break;
    }
    if (key.value()) {
      block->keys.push_back(*key.value());
    }
  }
  blocks_.emplace(blockId, block);
  while (blocks_.size() > kMaxBlocks) {
    blocks_.erase(blocks_.begin()); // the oldest: windows end at the tip
  }
  return block;
}

Roe<void> checkIdempotency(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    uint64_t idempotentId, uint64_t fromWalletId, uint64_t slotMin,
    uint64_t slotMax, const FnIdempotencyKeyForRecord &fnIdempotencyKeyForRecord,
    IdempotencyScanCache *cache) {
  if (idempotentId == 0) {
    return {};
  }
  if (cache) {
    return checkIdempotencyCached(ledger, consensus, idempotentId, fromWalletId, slotMin, slotMax,
                                  fnIdempotencyKeyForRecord, *cache);
  }
  const int64_t tsStart = consensus.getSlotStartTime(slotMin);
  auto startBlockRoe = ledger.findBlockByTimestamp(tsStart);
  if (!startBlockRoe) {
    return {};
  }
  const uint64_t nextBlockId = ledger.getNextBlockId();
  for (uint64_t blockId = startBlockRoe.value().block.index;
       blockId < nextBlockId; ++blockId) {
    auto blockRoe = ledger.readBlock(blockId);
    if (!blockRoe) {
      break;
    }
    const auto &block = blockRoe.value().block;
    if (block.slot > slotMax) {
      break;
    }
    if (block.slot < slotMin) {
      continue;
    }
    for (const auto &rec : block.records) {
      auto keyRoe = fnIdempotencyKeyForRecord(rec);
      if (!keyRoe) {
        return keyRoe.error();
      }
      const auto &optKey = keyRoe.value();
      if (!optKey.has_value()) {
        continue;
      }
      const auto &key = optKey.value();
      if (key.second == idempotentId && key.first == fromWalletId) {
        return duplicate(idempotentId, fromWalletId);
      }
    }
  }
  return {};
}

Roe<void> validateIdempotencyRules(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    uint64_t idempotentId, uint64_t fromWalletId, int64_t validationTsMin,
    int64_t validationTsMax, uint64_t effectiveSlot,
    chain_block::BlockAdmissionMode admissionMode,
    const FnIdempotencyKeyForRecord &fnIdempotencyKeyForRecord,
    bool requireNonZeroId, IdempotencyScanCache *cache) {
  if (!chain_block::admissionTxStrict(admissionMode)) {
    return {};
  }
  if (idempotentId == 0) {
    if (requireNonZeroId) {
      return TxError(chain_err::E_TX_IDEMPOTENCY,
                     "idempotentId must be non-zero for wallet: " +
                         std::to_string(fromWalletId));
    }
    return {};
  }
  if (validationTsMax < validationTsMin) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Validation window invalid: validationTsMax < validationTsMin");
  }
  if (!optChainConfig.has_value()) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Chain config not initialized; expected config in Full mode");
  }
  const uint64_t spanSeconds =
      static_cast<uint64_t>(validationTsMax - validationTsMin);
  const auto &config = optChainConfig.value();

  if (spanSeconds > config.maxValidationTimespanSeconds) {
    return TxError(chain_err::E_TX_VALIDATION_TIMESPAN,
                   "Validation window exceeds max timespan: " +
                       std::to_string(spanSeconds) + " > " +
                       std::to_string(config.maxValidationTimespanSeconds));
  }
  const uint64_t slotMin = consensus.getSlotFromTimestamp(validationTsMin);
  const uint64_t slotMax = consensus.getSlotFromTimestamp(validationTsMax);
  if (effectiveSlot < slotMin || effectiveSlot > slotMax) {
    return TxError(chain_err::E_TX_TIME_OUTSIDE_WINDOW,
                   "Current time (slot " + std::to_string(effectiveSlot) +
                       ") outside validation window [slot " +
                       std::to_string(slotMin) + ", " + std::to_string(slotMax) +
                       "]");
  }
  if (effectiveSlot == 0 || effectiveSlot <= slotMin) {
    return {};
  }
  const uint64_t slotMaxIdempotency = effectiveSlot - 1;
  return checkIdempotency(ledger, consensus, idempotentId, fromWalletId,
                          slotMin, slotMaxIdempotency, fnIdempotencyKeyForRecord, cache);
}

} // namespace pp::chain_tx
