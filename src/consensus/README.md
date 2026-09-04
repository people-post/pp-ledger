# Consensus Library

Live consensus for pp-ledger is `Ouroboros` (`Ouroboros.h` / `.cpp`): slot/epoch
clock, stakeholder cache, and deterministic slot-leader selection.

## Live leader election

Matches `Chain` / block validation (see [docs/wire-schema.md](../../docs/wire-schema.md)):

- Stakeholders refresh per epoch from account native balances.
- Eligible pool: all stakeholders if ≤100, else top 100 by stake.
- Leader index: `SHA-256("pp-ledger/ouroboros/v1:slot:N:epoch:M")` mod pool size
  (**equal weight** within the pool — not stake-proportional VRF).
- Blocks commit `epoch` and `stakeSnapshotHash` so the election inputs are
  verifiable from the header.

## Not on the live path

`SlotLeaderSelection` (simplified VRF / epoch nonce) and `EpochManager` remain
for experiments and unit tests. They are **not** wired into `Chain`. Product
docs that still say “stake-weighted VRF” describe a future direction, not the
current wire format.

## Configuration

Slot duration and slots per epoch come from genesis `BlockChainConfig`.
