#include "TxIdempotency.h"
#include "ErrorCodes.h"
#include "AccountBuffer.h"

namespace pp::chain_tx {

Roe<void> checkIdempotency(const Ledger &ledger,
                           const consensus::Ouroboros &consensus,
                           uint64_t idempotentId, uint64_t fromWalletId,
                           uint64_t slotMin, uint64_t slotMax) {
  if (idempotentId == 0) {
    return {};
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
      auto check = [&](uint64_t txWalletId, uint64_t txIdempotentId) -> Roe<void> {
        if (txIdempotentId == idempotentId && txWalletId == fromWalletId) {
          return TxError(
              chain_err::E_TX_IDEMPOTENCY,
              "Duplicate idempotent id: " + std::to_string(idempotentId) +
                  " for wallet: " + std::to_string(fromWalletId));
        }
        return {};
      };
      switch (rec.type) {
      case Ledger::T_DEFAULT: {
        auto txRoe = utl::binaryUnpack<Ledger::TxDefault>(rec.data);
        if (txRoe) { auto r = check(txRoe->fromWalletId, txRoe->idempotentId); if (!r) return r; }
        break;
      }
      case Ledger::T_NEW_USER: {
        auto txRoe = utl::binaryUnpack<Ledger::TxNewUser>(rec.data);
        if (txRoe) { auto r = check(txRoe->fromWalletId, txRoe->idempotentId); if (!r) return r; }
        break;
      }
      case Ledger::T_CONFIG: {
        auto txRoe = utl::binaryUnpack<Ledger::TxConfig>(rec.data);
        if (txRoe) { auto r = check(AccountBuffer::ID_GENESIS, txRoe->idempotentId); if (!r) return r; }
        break;
      }
      case Ledger::T_USER_UPDATE: {
        auto txRoe = utl::binaryUnpack<Ledger::TxUserUpdate>(rec.data);
        if (txRoe) { auto r = check(txRoe->walletId, txRoe->idempotentId); if (!r) return r; }
        break;
      }
      default:
        break;
      }
    }
  }
  return {};
}

Roe<void> validateIdempotencyRules(
    const Ledger &ledger, const consensus::Ouroboros &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    uint64_t idempotentId, uint64_t fromWalletId, int64_t validationTsMin,
    int64_t validationTsMax, uint64_t effectiveSlot, bool isStrictMode) {
  if (!isStrictMode) {
    return {};
  }
  if (idempotentId == 0) {
    return {};
  }
  if (validationTsMax < validationTsMin) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Validation window invalid: validationTsMax < validationTsMin");
  }
  if (!optChainConfig.has_value()) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Chain config not initialized; expected config in strict mode");
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
                          slotMin, slotMaxIdempotency);
}

} // namespace pp::chain_tx
