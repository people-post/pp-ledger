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

/** The most `maxBlockBytes` may be: what the ledger RPC carries in one message (B034). */
inline constexpr uint64_t kMaxBlockBytesCeiling = 4 * 1024 * 1024;
/** Room a record takes beyond its meta: the most keys and signatures an account may have, fields. */
inline constexpr uint64_t kRecordOverheadBytes = 256 * 1024;
/** Room a block takes beyond its records: header, committee. */
inline constexpr uint64_t kBlockOverheadBytes = 64 * 1024;

/**
 * Block size settings, at genesis init and in every T_CONFIG (B034): `maxBlockBytes` set, at most
 * `kMaxBlockBytesCeiling`, and room for the largest record (`maxCustomMetaSize` plus
 * `kRecordOverheadBytes`) in a block (plus `kBlockOverheadBytes`).
 */
Roe<void> validateBlockLimits(const BlockChainConfig &config);

} // namespace pp::chain_tx

#endif
