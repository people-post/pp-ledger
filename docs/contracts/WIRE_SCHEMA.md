# Ledger & block wire schema

Canonical description of the live account ledger and block/transaction wire
format. This document matches the code under `src/ledger/` and `src/chain/`.
There is **no backward compatibility** with earlier block versions; operators
must re-init work directories after schema bumps.

Related: [DESIGN.md](../product/DESIGN.md) (product overview), [LEDGER_STORAGE.md](LEDGER_STORAGE.md)
(on-disk volumes), [LEDGER_TOPOLOGY.md](../architecture/LEDGER_TOPOLOGY.md) (RPC / STATUS).

## Encoding

| Layer | Format |
|-------|--------|
| Consensus / disk | Binary LTS via `OutputArchive` / `InputArchive` (big-endian integers; length-prefixed strings) from pp-cpp-common |
| Nested payloads | `binaryPack` / `binaryUnpack` |
| Digests on wire | **Raw 32-byte** SHA-256 (`utl::sha256Raw`), not hex |
| JSON / HTTP / CLI | Display only; binary fields use `toJsonSafeString` (`0x` + hex when needed) |
| RPC frame | `u32` BE length + `Client::Request` / `Response` payload |

Genesis `previousHash` is `utl::zeroHash()` (32 zero bytes).

## Block

`Ledger::Block::CURRENT_VERSION` is the on-disk/header version prefix.

### Header (hashed)

Block hash = `SHA-256(headerToString())` where `headerToString` packs:

| Field | Type | Notes |
|-------|------|-------|
| `version` | `uint16` | `CURRENT_VERSION` |
| `index` | `uint64` | Height |
| `timestamp` | `int64` | Unix seconds; must fall in slot window |
| `previousHash` | 32 bytes | Parent digest |
| `slot` | `uint64` | Consensus slot |
| `slotLeader` | `uint64` | Account id elected for the slot |
| `epoch` | `uint64` | Must equal `epochFromSlot(slot)` |
| `txIndex` | `uint64` | Cumulative tx count before this block |
| `txRoot` | 32 bytes | Commitment to body |
| `stateRoot` | 32 bytes | Post-apply account state |
| `stakeSnapshotHash` | 32 bytes | Stake set used for leader election |
| `epochSeed` | 32 bytes | Epoch lottery seed (same for all blocks in an epoch) |

Full block LTS for disk also appends `records` after the header fields
(`Block::ltsToString`). Hash does **not** include `records` bytes directly.

### Body

`records`: ordered list of `Record` (see below).

### Commitments

| Root | Domain / rule |
|------|----------------|
| `txRoot` | `SHA-256("pp-ledger/txroot/v1" \|\| pack(each binaryPack(record)))` |
| `stateRoot` | O(1) root of the **account sparse Merkle tree** (depth 64 over `accountId`). Leaves are `SHA-256("pp-ledger/account-leaf/v1" \|\| pack(id, wallet, blockId))`. Updates are path-copied O(depth) per touched account — never a full-account scan, including at checkpoints. |
| `stakeSnapshotHash` | `SHA-256("pp-ledger/stake/v1" \|\| pack(id, stake)…)` stakeholders sorted by id |
| `epochSeed` | See [Epoch seed](#epoch-seed) |

`Chain::sealBlock` applies records once to the tip `AccountBuffer`, then sets
`stateRoot` and `hash`. The matching `addBlock` persists without re-applying.
Validation of received blocks still applies on `addBlock` and checks `stateRoot`
against the live tree root (O(1)).

Genesis: `index=slot=slotLeader=epoch=txIndex=0`, empty stake snapshot, genesis
`epochSeed`, four records (`T_GENESIS` + fee/reserve/recycle `T_NEW_USER`).

## Transaction record

```text
Record { type: uint16, data: bytes, signatures: bytes[] }
```

`data` is `binaryPack` of the typed payload (`TxDefault`, `TxGenesis`, …).

### Signing message

Signatures are ML-DSA-65 over:

```text
binaryPack(type, networkId, data)
```

(`Ledger::Record::makeSigningMessage`). `networkId` comes from
`BlockChainConfig.networkId` in genesis (also returned on STATUS when set).
Changing `type` or replaying onto another network invalidates signatures.

### Live types

| Id | Name | Role |
|----|------|------|
| 0 | `T_DEFAULT` | Transfer |
| 1 | `T_GENESIS` | Genesis config + genesis account |
| 2 | `T_NEW_USER` | Register / fund account |
| 3 | `T_CONFIG` | Config update |
| 4 | `T_USER_UPDATE` | Account self-update |
| 5 | `T_RENEWAL` | Miner renew |
| 6 | `T_END_USER` | Miner close |

Ids 7–15 are reserved for name-directory / attachment (`docs/product/NAME_DIRECTORY.md`);
not installed in `RecordHandler`.

## Account / ledger model

- **Account-based**, multi-token `map<tokenId, int64>` balances (not UTXO).
- Reserved accounts: Genesis `0`, Fee `1`, Reserve `2`, Recycle `3`; users `>= 1<<30`.
- Live state is an in-memory `AccountBuffer`; durable history is the block log.
- Checkpoints mark chain ranges from which account state can be reconstructed.

`BlockChainConfig` (embedded in genesis meta, `GenesisAccountMeta::VERSION` **3**)
includes slot timing, fees, checkpoint policy, `networkId`, and
`heartbeatSlots` (empty-seal lag; `0` disables).

## Epoch seed

Public 32-byte seed for the SlotCommittee lottery, committed on every block
header (`epochSeed`). Same value for all blocks in an epoch; derived at the
epoch boundary so leaders are not predictable from stake alone until the
previous epoch’s tip material is known.

Implemented in `consensus::EpochSeed` + `Chain::ensureEpochSeed` / `sealBlock`:

| Epoch | Derivation |
|-------|------------|
| `0` | `SHA-256("pp-ledger/epoch-seed/genesis/v1" \|\| len(networkId) \|\| networkId \|\| genesisConfigDigest \|\| 32×0)` |
| `E>0` | `SHA-256("pp-ledger/epoch-seed/v1" \|\| E \|\| len(networkId) \|\| networkId \|\| prevEpochSeed \|\| tipMaterial \|\| stakeSnapshotHash)` |

`tipMaterial` = lookback over up to **K=8** block hashes from epoch `E-1`
(oldest→newest): `SHA-256("pp-ledger/epoch-seed/lookback/v1" \|\| k \|\| hashes…)`.
If epoch `E-1` produced no blocks:
`SHA-256("pp-ledger/epoch-seed/empty-prev/v1" \|\| (E-1) \|\| prevEpochSeed)`.

Validators reject blocks whose `epochSeed` ≠ the locally derived/installed seed
for that epoch.

## Leader election (live)

Implemented in `consensus::SlotCommittee` (beacon-centered schedule; **not**
classic Ouroboros / stake-weighted VRF on blocks):

1. Stakeholders = accounts with positive native balance.
2. Eligible **committee** = all if ≤100, else top 100 by stake (id tie-break).
   Stake gates **entry** into the committee only.
3. Require `epochSeed` for the slot’s epoch (forced leaders in tests bypass).
4. Leader = committee member at index from the first 8 bytes (BE) of
   `SHA-256("pp-ledger/slot-committee/v2" \|\| u64be(slot) \|\| u64be(epoch) \|\| epochSeed)`
   modulo pool size (**equal weight** within the committee — **designed
   behavior**, not a temporary stand-in for stake-proportional sampling).

Blocks commit `epoch` + `stakeSnapshotHash` + `epochSeed` so verifiers can check
the election inputs. Demo VRF / `EpochNonce` under `SlotLeaderSelection` are
**not** on the live `Chain` path. Ouroboros is a literature reference only.

**Domain string note:** `slot-committee/v2` (binary domain + seed) replaces
`slot-committee/v1` string form and legacy `ouroboros/v1`; election outputs
differ. Block `CURRENT_VERSION` is **5** (adds `epochSeed`).

Open follow-ups (registration, production window, beacon failover, …):
[architecture/SLOT_COMMITTEE_OPEN_ITEMS.md](../architecture/SLOT_COMMITTEE_OPEN_ITEMS.md).

## Empty heartbeat blocks

When a slot leader has **no** renewals and **no** pending txs, they may still
seal a normal block with empty `records` if:

`currentSlot - tip.slot >= BlockChainConfig.heartbeatSlots`

(`heartbeatSlots == 0` disables). Default at beacon `--init` when the field is
omitted: `heartbeatSlots = slotsPerEpoch` (about one empty seal per idle epoch).

No special record type; fees are zero for the empty body. **Validators enforce**
the lag threshold on empty bodies (seal path + strict `validateNormalBlock`);
premature empties and all empties when `heartbeatSlots == 0` are rejected.
Leader, time window, and header commitments still apply. Policy helper:
`shouldSealEmptyHeartbeat` / `validateEmptyHeartbeatPolicy` in `chain/`; miner
gate in `Miner::produceBlock`.

## ChainNode / storage envelope

```text
ChainNode { block, hash }  // hash = SHA-256(header)
RawBlock  { data = Block::ltsToString(), hash }
```

Persisted via `VolumeStore` / `FileDirStore` / `FileStore` — see [LEDGER_STORAGE.md](LEDGER_STORAGE.md).
