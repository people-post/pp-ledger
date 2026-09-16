# Block pipeline

**Status:** normative for `Chain` admission / apply / persist  
**Code:** `chain/BlockValidation.*` (`checkBlock*`), `Chain::{sealBlock,addBlock,commitSealedBlock,loadFromLedger}`

Production, peer ingest, seal-commit, and ledger replay share one **apply** path
and one **admission** helper. Roles differ only by which stages they run and
which `BlockAdmissionMode` they select.

## Stages

| Stage | Mutates tip bank? | Persist? | Responsibility |
|-------|-------------------|----------|----------------|
| **Assemble** | no | no | Producer picks txs / renewals; fills slot, leader, timestamp |
| **Check** | no | no | `checkBlock` layers (mode-selected) |
| **Apply** | **once** | no | Tx handlers → tip `AccountBuffer`; seal computes `stateRoot` + hash |
| **Commit** | no | yes | Ledger write; clear `pendingSeal_` |

## Modes (`BlockAdmissionMode`)

| Mode | Layers | Tx apply | Used by |
|------|--------|----------|---------|
| **Full** | structural + consensus + body | strict | Tip ingest; live seal policy; genesis→tip replay (`startingBlockId == 0`) |
| **CheckpointReplay** | structural only | soft | `loadFromLedger` from checkpoint; live ingest while `currentId == lastId` |
| **SealedCommitVerify** | structural only | n/a (already applied) | `commitSealedBlock` after `sealBlock` |

Helpers: `admissionRunsConsensusAndBody`, `admissionTxStrict`.

Live tip selection: `Chain::admissionModeFor(index)` (replaces ad-hoc
`shouldUseStrictMode` boolean as the source of truth; the bool remains as
`admissionTxStrict(admissionModeFor(...))` for tx handlers).

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
| **Produce** | Assemble → miner gates (mempool / heartbeat UX) → `sealBlock` (consensus+body → Apply → hash) → broadcast → `addBlock` → `commitSealedBlock` (SealedCommitVerify + stateRoot) |
| **Peer / beacon ingest** | `addBlock` → Check(`admissionModeFor`) → Apply → Commit |
| **Replay** | `loadFromLedger` → Check(Full or CheckpointReplay) → Apply (no persist) |
| **Late join** | Mount at checkpoint → CheckpointReplay until tip advances past checkpoint → then Full |

Seal cannot run structural Check before Apply (hash needs `stateRoot`). It runs
consensus + body on the assembled header, Applies once, then commit runs
structural Check.

## Intentionally not duplicated

- Miner `produceBlock` empty-seal gate is UX only; admission still enforces
  heartbeat via body policy.
- Tx fee / signature / idempotency windows stay in handlers, keyed by
  `admissionTxStrict(mode)`.

## Related

- Wire / heartbeat: [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md)
- SlotCommittee open items: [SLOT_COMMITTEE_OPEN_ITEMS.md](SLOT_COMMITTEE_OPEN_ITEMS.md)
