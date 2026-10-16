# Block pipeline

**Status:** normative for `Chain` admission / apply / persist  
**Code:** `chain/BlockAdmission.h`, `chain/BlockValidation.*` (`checkBlock*`),
`Chain::{linkNextBlock,assembleBlockHeader,sealBlock,addBlock,commitSealedBlock,loadFromLedger}`,
`Miner::{shouldAttemptProduction,produceBlock,createBlock}`

Production, peer ingest, seal-commit, and ledger replay share one **apply** path
and one **admission** helper. Roles differ only by which stages they run and
which `BlockAdmissionMode` they select.

## Stages

| Stage | Mutates tip bank? | Persist? | Responsibility |
|-------|-------------------|----------|----------------|
| **Assemble** | no | no | (1) Producer: `linkNextBlock` — index / previousHash / slot / leader / timestamp / txIndex / records. (2) `assembleBlockHeader` (inside seal) — epoch / stake / seed / txRoot |
| **Check** | no | no | `checkBlock` layers (mode-selected) |
| **Apply** | **once** | no | Tx handlers → tip `AccountBuffer`; seal computes `stateRoot` + hash |
| **Commit** | no | yes | Ledger write; clear `pendingSeal_` |

`sealBlock` = `assembleBlockHeader` → Check (non-genesis Full) → Apply (**always Full**) → pending seal.  
Seal is refused while `admissionModeFor` is `CheckpointReplay` (catch-up mount).  
`createBlock` / tests: `linkNextBlock` then `sealBlock`.  
On Check/Apply failure after Assemble, the caller's `ChainNode` may retain
partial header fields; callers should discard it (miner builds a fresh node).

## Modes (`BlockAdmissionMode`)

| Mode | Layers | Tx apply | Used by |
|------|--------|----------|---------|
| **Full** | structural + consensus + body | strict | Tip ingest; **all seals**; genesis→tip replay (`startingBlockId == 0`) |
| **CheckpointReplay** | structural (incl. the committee field) | soft: effects on held accounts | `loadFromLedger` from checkpoint; live ingest while `currentId == lastId` |
| **SealedCommitVerify** | structural only | n/a (already applied) | `commitSealedBlock` after `sealBlock` |

Helpers: `admissionRunsConsensusAndBody`, `admissionTxStrict`.

Live tip selection: `Chain::admissionModeFor(index)`. Tx apply contexts carry
the same `BlockAdmissionMode` (`BufferApplyContext` / `BlockApplyContext`);
handlers call `admissionTxStrict(mode)` instead of a parallel bool.

## Checkpoints and late join

A **checkpoint** is where a miner can start (beacon-authority B015). Block *C* becomes the next
checkpoint when (`chain_block::rotatesCheckpoint`, from stored blocks and slot times only):

- it is the **first block of its epoch**;
- `C ≥ currentId + checkpoint.minBlocks`;
- the block at `currentId` is more than `checkpoint.minAgeSeconds` older than the start of the slot
  of block *C−1*.

Then every account last written before `currentId` was due for renewal by block *C−1*
(`calculateMaxBlockIdForRenewal` uses the block's slot time, never a node's clock). The checkpoint
block **opens with the genesis renewal** (due or not): config, pending update, admin set and
authority. And **every epoch's first block carries its committee** (`Block::committee`, the
preimage of `stakeSnapshotHash`, checked structurally in every mode). The newest two checkpoints
are kept: `lastId` (where a miner starts) and `currentId`.

1. A miner mounts with `startingBlockId = checkpointId` (its upstream's `lastId`) — a relay always
   mounts from block 0 and validates the whole chain in Full mode.
2. **Warm-up** (`CheckpointReplay`, while `currentId == lastId`; `loadFromLedger` replays the same
   way after a restart): the starting block's genesis renewal installs config, consensus,
   authority and the genesis account (`Chain::installCheckpointState`); each epoch's committee
   comes from its first block, so leaders and liveness charges are exact; every effect lands on
   the accounts the node holds (`chain_tx::applyTransferToHeldAccounts` and the handlers' soft
   paths), others arrive with their renewals; no `stateRoot` check and no consensus/body layers.
3. Every account renews before the current checkpoint. From it (`index >= currentId` after the
   rotation) blocks are **Full** — `stateRoot` and the stake snapshot (computed from the node's own
   state) must match, which proves the warm-up exact.
4. **Produce/seal** (`Chain::sealReadiness`) requires Full, and blocks held back over the longest
   transaction validity window (`maxValidationTimespanSeconds`, config in force): idempotency is
   checked against held blocks, so a transaction committed before the node's first block could
   otherwise be included again. With the defaults (checkpoints at least a year apart, a one-day
   window) this never delays a join.

Warm-up applies **every effect on the accounts the node holds**, whoever else a record touches:
a transfer's payer debit, payee credit and fee credit each land if that account is held
(`chain_tx::applyTransferToHeldAccounts`); a new account is created whether or not its payer is
held; a miner record's fee reaches a held fee account; an ended account's balances, as its
`T_END_USER` record carries them, reach a held recycle account; the checkpoint block's liveness
charges start at its epoch's first slot (its previous block is not held). Renewals of held
accounts keep their balance and miner-state checks: they expose a diverged state instead of
hiding it.

Warm-up trusts its blocks: they come from a beacon-signed range
([BEACON_AUTHORITY.md § Adoption rule](../contracts/BEACON_AUTHORITY.md#adoption-rule)) and are not
re-validated as Full (open discussion A).

## Layers (`checkBlock*`)

1. **Structural** — `txRoot`, header hash, sequence / `previousHash` / `txIndex`
2. **Consensus** — epoch, `stakeSnapshotHash`, `epochSeed`, slot leader, slot timing  
   (`validateBlockTiming`: timestamp in `[slotStart, slotStart + slotDuration)`)
3. **Body policy** — renewals, `maxTransactionsPerBlock`, empty heartbeat,
   intra-block idempotency

`checkBlock(mode)` runs structural always; consensus + body only for **Full**.

`checkBlockArrival` (slot > previous slot; slot <= current slot +
max(2 slots, 15 s)) runs only for new blocks: before seal applies anything and
in `Chain::addBlock` under Full. `loadFromLedger` and CheckpointReplay catch-up
skip it, so history written before these rules still loads.

### Empty heartbeat (settled: any work counts)

`heartbeatSlots` rate-limits **empty** seals only (`records` empty). Goal: prove
the chain is **active**, not that idle leaders must emit empties on a duty cycle.

- Any non-empty body (mempool txs **or renewals**) bypasses the empty-lag rule —
  tip advance already shows liveness.
- Empty bodies still need `currentSlot - tip.slot >= heartbeatSlots`
  (`0` disables the lag rule) — unless the producer is a **due miner**, whose
  empty block is always admitted ([Miner registration and liveness](../contracts/WIRE_SCHEMA.md#miner-registration-and-liveness)).
- Anti-spam stays in Full admission (fees, size, signatures), not in heartbeat.
- Ops / STATUS liveness should prefer **last tip advance**, not “last empty
  heartbeat,” because renewals-only or mempool blocks can move the tip without
  an empty seal.

Normative wire text: [WIRE_SCHEMA.md — Empty heartbeat blocks](../contracts/WIRE_SCHEMA.md#empty-heartbeat-blocks).

## Protocol constants (fork-critical if changed)

Live code keeps these as **compile-time** constants (not `BlockChainConfig`):

| Constant | Value | Location |
|----------|-------|----------|
| Committee max pool size | `100` | `SlotCommittee::kMaxLeaderPoolSize` |
| Epoch-seed tip lookback | `8` | `consensus::kEpochSeedLookback` |
| Most empty epochs a seed chains through | `1024` | `consensus::kMaxEmptyEpochs` |

If either is ever exposed as a network knob, it **must** move into genesis
`BlockChainConfig` (and bump `GenesisAccountMeta` / wire docs): diverging values
fork leader election / `epochSeed`. Prefer leaving them hardcoded until then.

## Role map

| Role | Pipeline |
|------|----------|
| **Produce** | txs → `linkNextBlock` → UX `shouldAttemptProduction` → `sealBlock` → broadcast → `addBlock` → `commitSealedBlock` |
| **Peer / beacon ingest** | `addBlock` → Check(`admissionModeFor`) → Apply → Commit |
| **Replay** | `loadFromLedger` → Check(Full or CheckpointReplay) → Apply (no persist) |
| **Late join** | Mount at the previous checkpoint → CheckpointReplay (warm-up) until the current checkpoint → then Full; no seal until Full; warm-up not re-Full-verified |

Seal cannot run structural Check before Apply (hash needs `stateRoot`). It runs
consensus + body on the assembled header, Applies once, then commit runs
structural Check.

## Intentionally not duplicated

- `Miner::shouldAttemptProduction` (empty heartbeat + seal window) is the leader's own gate;
  admission enforces heartbeat via `checkBlockBodyPolicy`, the timestamp via
  `validateBlockTiming`, and the beacon the delivery deadline.
- Tx fee / signature / idempotency windows stay in handlers, keyed by
  `admissionTxStrict(admissionMode)` on the apply context.

## Open design discussions (deferred)

Capture for a later design pass — not scheduled implementation.

### A. CheckpointReplay trust model

Soft catch-up skips consensus/body and may soft-skip signatures when the signer
account is missing. Its blocks now come only from a range a verified beacon signed tip vouches for
(beacon-authority ba4/ba5b), and the first Full block checks `stateRoot` and the stake snapshot,
so a divergent warm-up stops the node instead of misleading it. What remains is trust in the beacon
for the warm-up's contents: a compromised beacon could plant state there. Alternatives: require
Full after N blocks, attested checkpoint blobs.

### B. `T_CONFIG` limits for liveness settings — **settled**

`heartbeatSlots ≤ slotsPerEpoch`; `minerDueAfterSlots` is `0` or at least 12 hours of slots —
at beacon `--init` and in every `T_CONFIG`. Normative:
[WIRE_SCHEMA.md — Config updates](../contracts/WIRE_SCHEMA.md#config-updates).

### C. Slot production window — **settled**

Leaders seal after their slot ends and deliver within `min(slot / 3, 2 s)`; the beacon refuses
late blocks. Normative: [WIRE_SCHEMA.md — Slot timing](../contracts/WIRE_SCHEMA.md#slot-timing).

### D. `T_CONFIG` timing fields — **settled: fixed at genesis**

`slotDuration` and `slotsPerEpoch` cannot change after genesis: a `T_CONFIG` changing either is
refused, so the stored config and the slot schedule (`SlotCommittee::Config`, set once at init)
always agree. Normative: [WIRE_SCHEMA.md — Config updates](../contracts/WIRE_SCHEMA.md#config-updates).

### E. Genesis renewal re-records config — **settled**

A genesis `T_RENEWAL` must carry the config in force for its block plus any pending update
(separately); validators refuse a mismatch. Normative:
[WIRE_SCHEMA.md — Config updates](../contracts/WIRE_SCHEMA.md#config-updates).

## Related

- Wire / heartbeat: [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md)
- SlotCommittee open items: [SLOT_COMMITTEE_OPEN_ITEMS.md](SLOT_COMMITTEE_OPEN_ITEMS.md)
