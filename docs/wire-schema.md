# Ledger & block wire schema

Canonical description of the live account ledger and block/transaction wire
format. This document matches the code under `src/ledger/` and `src/chain/`.
There is **no backward compatibility** with earlier block versions; operators
must re-init work directories after schema bumps.

Related: [design.md](design.md) (product overview), [ledger-storage.md](ledger-storage.md)
(on-disk volumes), [ledger-topology.md](ledger-topology.md) (RPC / STATUS).

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

`Chain::sealBlock` applies records once to the tip `AccountBuffer`, then sets
`stateRoot` and `hash`. The matching `addBlock` persists without re-applying.
Validation of received blocks still applies on `addBlock` and checks `stateRoot`
against the live tree root (O(1)).

Genesis: `index=slot=slotLeader=epoch=txIndex=0`, empty stake snapshot, four records
(`T_GENESIS` + fee/reserve/recycle `T_NEW_USER`).

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

Ids 7–15 are reserved for name-directory / attachment (`docs/name-directory.md`);
not installed in `RecordHandler`.

## Account / ledger model

- **Account-based**, multi-token `map<tokenId, int64>` balances (not UTXO).
- Reserved accounts: Genesis `0`, Fee `1`, Reserve `2`, Recycle `3`; users `>= 1<<30`.
- Live state is an in-memory `AccountBuffer`; durable history is the block log.
- Checkpoints mark chain ranges from which account state can be reconstructed.

`BlockChainConfig` (embedded in genesis meta, `GenesisAccountMeta::VERSION`) includes
slot timing, fees, checkpoint policy, and `networkId`.

## Leader election (live)

Implemented in `consensus::Ouroboros` (not VRF proofs on blocks):

1. Stakeholders = accounts with positive native balance.
2. Eligible pool = all if ≤100, else top 100 by stake (id tie-break).
3. Leader = pool member at index `SHA-256("pp-ledger/ouroboros/v1:slot:N:epoch:M")` mod pool size (**equal weight** within the pool).

Blocks commit `epoch` + `stakeSnapshotHash` so verifiers can check the election
inputs. Demo VRF / `EpochNonce` under `SlotLeaderSelection` are **not** on the
live `Chain` path.

## ChainNode / storage envelope

```text
ChainNode { block, hash }  // hash = SHA-256(header)
RawBlock  { data = Block::ltsToString(), hash }
```

Persisted via `VolumeStore` / `FileDirStore` / `FileStore` — see [ledger-storage.md](ledger-storage.md).
