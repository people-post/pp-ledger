# Consensus Library

Live consensus for pp-ledger is **`SlotCommittee`** (`SlotCommittee.h` / `.cpp`):
slot/epoch clock, stakeholder cache, epoch lottery seed, and deterministic
slot-leader election under beacon authority.

## Live leader election (designed)

Matches `Chain` / block validation (see
[docs/contracts/WIRE_SCHEMA.md](../../docs/contracts/WIRE_SCHEMA.md)):

- Stakeholders refresh per epoch from account native balances.
- Eligible **committee**: all positive-stake accounts if ≤100, else **top 100 by
  stake** (id ascending tie-break).
- **Epoch seed** (`EpochSeed.h`): 32-byte header field; genesis from network id +
  config digest; later epochs from prior seed + lookback tip material + stake
  snapshot.
- **Equal weight** within the committee (intentional): leader index from
  `SHA-256("pp-ledger/slot-committee/v2" || slot || epoch || epochSeed)`
  (first 8 bytes BE) mod pool size. Stake gates *entry* to the committee, not
  weight inside it.
- Blocks commit `epoch`, `stakeSnapshotHash`, and `epochSeed` so the election
  inputs are verifiable from the header.

This is **not** classic Ouroboros (stake-weighted VRF / empty-slot probability).
Ouroboros remains a literature reference for comparing designs.

## Not on the live path

`SlotLeaderSelection` (simplified VRF / epoch nonce) and `EpochManager` remain
for experiments and unit tests. They are **not** wired into `Chain`.

## Configuration

Slot duration and slots per epoch come from genesis `BlockChainConfig`.

## Open decisions

Deferred product/protocol choices (miner-registered committee, production window,
empty blocks, beacon failover, …):
[docs/architecture/SLOT_COMMITTEE_OPEN_ITEMS.md](../../docs/architecture/SLOT_COMMITTEE_OPEN_ITEMS.md).

Doc map: [docs/README.md](../../docs/README.md).
