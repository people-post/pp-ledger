# Block pipeline

**Status:** normative for `Chain` admission / apply / persist  
**Code:** `chain/BlockAdmission.h`, `chain/BlockValidation.*` (`checkBlock*`),
`Chain::{assembleBlockHeader,sealBlock,addBlock,commitSealedBlock,loadFromLedger}`,
`Miner::{shouldAttemptProduction,produceBlock,createBlock}`

Production, peer ingest, seal-commit, and ledger replay share one **apply** path
and one **admission** helper. Roles differ only by which stages they run and
which `BlockAdmissionMode` they select.

## Stages

| Stage | Mutates tip bank? | Persist? | Responsibility |
|-------|-------------------|----------|----------------|
| **Assemble** | no | no | Producer picks txs; fills slot/leader/timestamp; `assembleBlockHeader` fills epoch / stake / seed / txRoot |
| **Check** | no | no | `checkBlock` layers (mode-selected) |
| **Apply** | **once** | no | Tx handlers → tip `AccountBuffer`; seal computes `stateRoot` + hash |
| **Commit** | no | yes | Ledger write; clear `pendingSeal_` |

`sealBlock` = Assemble → Check (non-genesis Full layers) → Apply → pending seal.
`createBlock` fills index/links then calls `sealBlock`.

## Modes (`BlockAdmissionMode`)

| Mode | Layers | Tx apply | Used by |
|------|--------|----------|---------|
| **Full** | structural + consensus + body | strict | Tip ingest; live seal policy; genesis→tip replay (`startingBlockId == 0`) |
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

This is intentional softness for catch-up, not a second validation engine.

## Layers (`checkBlock*`)

1. **Structural** — `txRoot`, header hash, sequence / `previousHash` / `txIndex`
2. **Consensus** — epoch, `stakeSnapshotHash`, `epochSeed`, slot leader, slot timing  
   (`validateBlockTiming`: timestamp in `[slotStart, slotStart + slotDuration)`)
3. **Body policy** — renewals, `maxTransactionsPerBlock`, empty heartbeat,
   intra-block idempotency

`checkBlock(mode)` runs structural always; consensus + body only for **Full**.

## Role map

| Role | Pipeline |
|------|----------|
| **Produce** | Assemble (miner txs + `createBlock`) → UX gate `shouldAttemptProduction` → `sealBlock` (Assemble header + Check + Apply) → broadcast → `addBlock` → `commitSealedBlock` |
| **Peer / beacon ingest** | `addBlock` → Check(`admissionModeFor`) → Apply → Commit |
| **Replay** | `loadFromLedger` → Check(Full or CheckpointReplay) → Apply (no persist) |
| **Late join** | Mount at checkpoint → CheckpointReplay until tip advances past checkpoint → then Full |

Seal cannot run structural Check before Apply (hash needs `stateRoot`). It runs
consensus + body on the assembled header, Applies once, then commit runs
structural Check.

## Intentionally not duplicated

- `Miner::shouldAttemptProduction` (empty heartbeat + production window) is UX only;
  admission still enforces heartbeat via `checkBlockBodyPolicy`.
- Tx fee / signature / idempotency windows stay in handlers, keyed by
  `admissionTxStrict(admissionMode)` on the apply context.

## Related

- Wire / heartbeat: [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md)
- SlotCommittee open items: [SLOT_COMMITTEE_OPEN_ITEMS.md](SLOT_COMMITTEE_OPEN_ITEMS.md)
