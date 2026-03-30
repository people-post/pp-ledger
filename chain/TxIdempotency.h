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
    uint64_t idempotentId, uint64_t fromWalletId, int64_t validationTsMin,
    int64_t validationTsMax, uint64_t effectiveSlot, bool isStrictMode);

} // namespace pp::chain_tx

#endif
