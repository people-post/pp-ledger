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
| **CheckpointReplay** | structural only | soft | `loadFromLedger` from checkpoint; live ingest while `currentId == lastId` |
| **SealedCommitVerify** | structural only | n/a (already applied) | `commitSealedBlock` after `sealBlock` |

Helpers: `admissionRunsConsensusAndBody`, `admissionTxStrict`.

Live tip selection: `Chain::admissionModeFor(index)`. Tx apply contexts carry
the same `BlockAdmissionMode` (`BufferApplyContext` / `BlockApplyContext`);
handlers call `admissionTxStrict(mode)` instead of a parallel bool.

## Late join (mode matrix)

1. Miner/relay mounts with `startingBlockId = checkpointId` from beacon STATUS.
2. `loadFromLedger(startingBlockId)` sets `lastId == currentId == startingBlockId`
   and replays with **CheckpointReplay** (structural + soft txs).
3. While `currentId == lastId`, live `addBlock` also uses **CheckpointReplay**
   (`admissionModeFor`) — soft sync catch-up after mount.
4. After checkpoint rotation advances `currentId` past `lastId`, blocks with
   `index >= currentId` use **Full**.
5. **Produce/seal** requires Full; soft catch-up must not mint tip state.

**Trust:** CheckpointReplay skips consensus/body and may soft-skip signatures when
the signer account is missing. Safe only if catch-up blocks come from a trusted
beacon (or equivalent). Do not treat arbitrary peer feed as Full-equivalent.

## Layers (`checkBlock*`)

1. **Structural** — `txRoot`, header hash, sequence / `previousHash` / `txIndex`
2. **Consensus** — epoch, `stakeSnapshotHash`, `epochSeed`, slot leader, slot timing  
   (`validateBlockTiming`: timestamp in `[slotStart, slotStart + slotDuration)`)
3. **Body policy** — renewals, `maxTransactionsPerBlock`, empty heartbeat,
   intra-block idempotency

`checkBlock(mode)` runs structural always; consensus + body only for **Full**.

Empty-heartbeat lag applies only when `records` is empty; any non-empty body
bypasses that rate limit (renewals / mempool work). That is intentional
(liveness signal: chain is active).

## Protocol constants (fork-critical if changed)

Live code keeps these as **compile-time** constants (not `BlockChainConfig`):

| Constant | Value | Location |
|----------|-------|----------|
| Committee max pool size | `100` | `SlotCommittee::kMaxLeaderPoolSize` |
| Epoch-seed tip lookback | `8` | `consensus::kEpochSeedLookback` |

If either is ever exposed as a network knob, it **must** move into genesis
`BlockChainConfig` (and bump `GenesisAccountMeta` / wire docs): diverging values
fork leader election / `epochSeed`. Prefer leaving them hardcoded until then.

## Role map

| Role | Pipeline |
|------|----------|
| **Produce** | txs → `linkNextBlock` → UX `shouldAttemptProduction` → `sealBlock` → broadcast → `addBlock` → `commitSealedBlock` |
| **Peer / beacon ingest** | `addBlock` → Check(`admissionModeFor`) → Apply → Commit |
| **Replay** | `loadFromLedger` → Check(Full or CheckpointReplay) → Apply (no persist) |
| **Late join** | Mount at checkpoint → CheckpointReplay until tip advances past checkpoint → then Full; no seal until Full |

Seal cannot run structural Check before Apply (hash needs `stateRoot`). It runs
consensus + body on the assembled header, Applies once, then commit runs
structural Check.

## Intentionally not duplicated

- `Miner::shouldAttemptProduction` (empty heartbeat + production window) is UX only;
  admission still enforces heartbeat via `checkBlockBodyPolicy`.
- Tx fee / signature / idempotency windows stay in handlers, keyed by
  `admissionTxStrict(admissionMode)` on the apply context.

## Open design discussions (deferred)

Capture for a later design pass — not scheduled implementation.

### A. CheckpointReplay trust model

Soft catch-up skips consensus/body and may soft-skip signatures when the signer
account is missing. Today that assumes a **trusted beacon** (or equivalent) feed.
Alternatives if peers can supply catch-up blocks: require Full after N blocks,
attested checkpoint blobs, or a distinct `TrustedReplay` mode with explicit
source binding. See Late join above.

### B. `T_CONFIG` mutation policy for `heartbeatSlots` (and kin)

`T_CONFIG` can replace `heartbeatSlots` (including `0` / `1`) with no
monotonicity or delay. Decide whether liveness knobs are free governance
updates, need floors/ceilings, or epoch-delayed activation — same class of
question as other mutable `BlockChainConfig` fields.

### C. Slot production window

`isSlotBlockProductionTime` is still ~last 1s of the slot (local UX). Open
product choice: widen for ops/smoke, keep local-only, or (only if mandated
network-wide) put a window parameter in chain config. See also
[SLOT_COMMITTEE_OPEN_ITEMS.md](SLOT_COMMITTEE_OPEN_ITEMS.md) item B.

## Related

- Wire / heartbeat: [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md)
- SlotCommittee open items: [SLOT_COMMITTEE_OPEN_ITEMS.md](SLOT_COMMITTEE_OPEN_ITEMS.md)
