# Consensus Library

Live consensus for pp-ledger is **`SlotCommittee`** (`SlotCommittee.h` / `.cpp`):
slot/epoch clock, stakeholder cache, and deterministic slot-leader election under
beacon authority.

## Live leader election (designed)

Matches `Chain` / block validation (see [docs/wire-schema.md](../../docs/wire-schema.md)):

- Stakeholders refresh per epoch from account native balances.
- Eligible **committee**: all positive-stake accounts if ≤100, else **top 100 by
  stake** (id ascending tie-break).
- **Equal weight** within the committee (intentional): leader index =
  `SHA-256("pp-ledger/slot-committee/v1:slot:N:epoch:M")` mod pool size.
  Stake gates *entry* to the committee, not weight inside it.
- Blocks commit `epoch` and `stakeSnapshotHash` so the election inputs are
  verifiable from the header.

This is **not** classic Ouroboros (stake-weighted VRF / empty-slot probability).
Ouroboros remains a literature reference for comparing designs.

## Not on the live path

`SlotLeaderSelection` (simplified VRF / epoch nonce) and `EpochManager` remain
for experiments and unit tests. They are **not** wired into `Chain`.

## Configuration

Slot duration and slots per epoch come from genesis `BlockChainConfig`.
