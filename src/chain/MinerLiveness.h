#ifndef PP_LEDGER_MINER_LIVENESS_H
#define PP_LEDGER_MINER_LIVENESS_H

#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/TxContext.h"
#include "ledger/chain/Types.h"
#include "ledger/client/Client.h"
#include "ledger/consensus/SlotCommittee.h"
#include "ledger/ledger/Ledger.h"

#include <cstdint>
#include <vector>

namespace pp::chain_tx {

/**
 * Miner liveness (docs/contracts/WIRE_SCHEMA.md § Miner registration and liveness). A registered miner whose
 * last block (or registration) is `minerDueAfterSlots` slots old is due: in a slot it leads it
 * must produce, an empty block if need be. Never due when the setting is 0.
 */
bool isMinerDue(const Client::MinerState &miner, uint64_t slot, const BlockChainConfig &config);

/**
 * Block-level miner bookkeeping, after the block's records apply (so a renewal in the block
 * carries the pre-block state), on every node:
 *  1. each slot the block skipped, in the block's epoch (after `prevSlot`), whose elected leader
 *     was a due registered miner counts a miss for it; `minerMaxMissedSlots` in a row
 *     deregisters it (cool-down epoch recorded) — it leaves the committee at the next snapshot.
 *     Not when `chargeSkipped` is false: the block after a beacon outage (it commits a certificate
 *     with a cutover, bp3) — the gap is the beacon's, not the leaders';
 *  2. a registered producer's last produced slot is the block's slot and its misses reset.
 * `consensus` must hold the block's epoch committee and seed. Accounts missing from `bank`
 * (late-join replay) are skipped when `strict` is false. `charged`, when given, gets each miss's
 * leader (in slot order).
 */
Roe<void> applyMinerLiveness(AccountBuffer &bank, const consensus::SlotCommittee &consensus,
                             const BlockChainConfig &config, uint64_t prevSlot, const Ledger::Block &block,
                             bool strict, bool chargeSkipped = true, std::vector<uint64_t> *charged = nullptr);

} // namespace pp::chain_tx

#endif
