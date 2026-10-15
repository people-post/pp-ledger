#ifndef PP_LEDGER_CONFIG_POLICY_H
#define PP_LEDGER_CONFIG_POLICY_H

#include "TxError.h"
#include "Types.h"

#include <cstdint>

namespace pp::chain_tx {

/** Shortest miner due period (`minerDueAfterSlots × slotDuration`), in seconds: 12 hours. */
inline constexpr uint64_t kMinMinerDueSeconds = 12 * 3600;

/**
 * Liveness settings a chain config may hold, at genesis init and in every T_CONFIG
 * (WIRE_SCHEMA.md § Config updates):
 *  - `heartbeatSlots` ≤ `slotsPerEpoch` (0 = no heartbeats), so an idle chain still gets a
 *    block every epoch;
 *  - `minerDueAfterSlots` 0 (never due) or at least `kMinMinerDueSeconds` of slots, so miners
 *    are not due — owing an empty block — in nearly every slot they lead.
 */
Roe<void> validateLivenessSettings(const BlockChainConfig &config);

} // namespace pp::chain_tx

#endif
