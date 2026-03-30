#ifndef PP_LEDGER_TX_IDEMPOTENCY_H
#define PP_LEDGER_TX_IDEMPOTENCY_H

#include "TxError.h"
#include "Types.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "TxFees.h"

#include <cstdint>
#include <optional>

namespace pp::chain_tx {

Roe<void> checkIdempotency(const Ledger &ledger,
                           const consensus::Ouroboros &consensus,
                           uint64_t idempotentId, uint64_t fromWalletId,
                           uint64_t slotMin, uint64_t slotMax);

Roe<void> validateIdempotencyRules(
    const Ledger &ledger, const consensus::Ouroboros &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const TxView &tx, uint64_t effectiveSlot, bool isStrictMode);

template <typename TxT>
Roe<void> validateIdempotencyRules(
    const Ledger &ledger, const consensus::Ouroboros &consensus,
    const std::optional<BlockChainConfig> &optChainConfig, const TxT &tx,
    uint64_t effectiveSlot, bool isStrictMode) {
  return validateIdempotencyRules(ledger, consensus, optChainConfig, makeTxView(tx),
                                  effectiveSlot, isStrictMode);
}

} // namespace pp::chain_tx

#endif
