#ifndef PP_LEDGER_CHAIN_TX_IDEMPOTENCY_H
#define PP_LEDGER_CHAIN_TX_IDEMPOTENCY_H

#include "ChainTxError.h"
#include "ChainTypes.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"

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
    const Ledger::Transaction &tx, uint64_t effectiveSlot, bool isStrictMode);

} // namespace pp::chain_tx

#endif
