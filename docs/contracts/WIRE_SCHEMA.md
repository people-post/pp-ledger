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
| `timestamp` | `int64` | Unix seconds; the seal time, in `[slotEnd, slotEnd + window)` ([Slot timing](#slot-timing)) |
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
`epochSeed`, then records: `T_GENESIS`, then `T_NEW_USER` for fee, reserve,
registrar and recycle (in that order: fee first, since the others pay their fees
into it; only reserve is funded), and any **genesis miners** — `T_NEW_USER` from
genesis into the issued range with a positive stake and the exact fee. Reserve
amount + all stakes + all fees equal the initial supply. Genesis miners are the stake at start: system accounts never count.

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
| 16 | `T_MINER_REGISTER` | Account registers as a miner ([Miner registration and liveness](#miner-registration-and-liveness)) |
| 17 | `T_MINER_REVOKE` | Account revokes its miner registration |

Ids 7–15 are reserved for name-directory / attachment (`docs/product/NAME_DIRECTORY.md`);
not installed in `RecordHandler`.

### Config updates

`T_CONFIG` (signed by the genesis quorum, fee `0`) carries a full
`GenesisAccountMeta`: the next `BlockChainConfig` and the genesis account.

- **The config applies at the next epoch boundary.** A `T_CONFIG` in a block of
  epoch *e* is pending until the first block of epoch *e + 1*, which is checked
  and applied under the new config. Everyone sees a change for the rest of its
  epoch before it binds.
- **One pending update at a time.** A `T_CONFIG` is refused while another is
  pending, so at most one config takes effect per epoch. Limits held in the
  config follow the same rule, so a raised limit cannot be used in the epoch
  that raised it.
- **The genesis account (keys, rotation) is replaced at once.**
- Fields that may not move: `genesisTime`, `slotDuration` and `slotsPerEpoch` — slot timing
  is fixed at genesis (the slot schedule is set once from it; slot and epoch numbering stay
  continuous for every node).
- **Liveness limits** (beacon `--init` and every `T_CONFIG`): `heartbeatSlots ≤ slotsPerEpoch`
  (`0` = no heartbeats), so an idle chain still gets a block every epoch; `minerDueAfterSlots`
  is `0` (never due) or at least 12 hours of slots (`⌈43200 / slotDuration⌉`), so miners do not
  owe an empty block in nearly every slot they lead. Code: `chain_tx::validateLivenessSettings`.
- **A genesis renewal carries the config state.** Genesis meta (`GenesisAccountMeta`) is
  `config`, `genesis`, and a pending section (`hasPending`, `pending` = activation epoch +
  config). `T_GENESIS` and `T_CONFIG` carry no pending section (a `T_CONFIG`'s `config` is
  the update). A genesis `T_RENEWAL` — what a node starting from its checkpoint reads — must
  carry the config in force for its block and, if an update is still pending (its epoch has
  not come), that update in the pending section; validators refuse anything else.

## Account / ledger model

- **Account-based**, multi-token `map<tokenId, int64>` balances (not UTXO).
- Account id ranges (`AccountIds.h`):

  | Range | Kind | Created by | Rules |
  |-------|------|------------|-------|
  | `[0, 1<<20)` | **System** — Genesis `0`, Reserve `1`, Registrar `2`, Fee `3`, Recycle `4`; rest reserved | Genesis block only | Never terminated (`T_END_USER` refused); renewal fee must be `0` (genesis included: it issues the native token, so a fee would be minted) |
  | `[1<<20, 1<<30)` | **Issued** (token issuers, operator-created accounts) | Registrar | Normal renewal / termination |
  | `[1<<30, …)` | **Users** | Any funded account except genesis | Normal renewal / termination |

  Every `T_NEW_USER` after genesis pays at least the meta-size fee plus the
  one-time `newAccountFee` (chain config, may be `0`); genesis-block records pay
  exactly the meta-size fee. Fees are paid in advance: the meta-size fee at
  creation (the same formula as renewal) covers the first period, until the
  account's renewal falls due; each renewal pays for the period after it.

  System ids follow a token's life: minted (genesis), held (reserve), spent on
  new issued accounts (registrar), collected (fee, recycle). After genesis the
  genesis account creates no accounts and transfers only to reserve: every
  transfer out of genesis mints, so new supply has one path (genesis → reserve).
  Net issuance per epoch (amount + fee) is capped by `maxIssuancePerEpoch`,
  measured against genesis's balance at the epoch's first block.

  System ids 5 … 2²⁰−1 are reserved and **no rule creates them**: the genesis
  block can hold only the four system accounts above (records 1–4) and genesis miners
  (issued range), and `NEW_USER` into the system range is refused after
  genesis. Adding a system account is a protocol change — a constant in
  `AccountIds.h` plus code for its role and an explicit creation rule (e.g. a
  `T_CONFIG` from the genesis quorum at an activation epoch) — not a config
  setting.

- Renewal re-records an account's state in a recent block so old blocks can be
  dropped after a checkpoint; every account, system ones included, must renew
  when due.
- Live state is an in-memory `AccountBuffer`; durable history is the block log.
- Checkpoints mark chain ranges from which account state can be reconstructed.

`BlockChainConfig` (embedded in genesis meta, `GenesisAccountMeta::VERSION` **7**)
includes slot timing, fees, checkpoint policy, `networkId`, `heartbeatSlots`
(empty-seal lag; `0` disables), issuance and account fees, and the miner settings
([Miner registration and liveness](#miner-registration-and-liveness)).

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

`tipMaterial` = lookback over up to **K=`kEpochSeedLookback` (8)** block hashes
from epoch `E-1` (oldest→newest):
`SHA-256("pp-ledger/epoch-seed/lookback/v1" || k || hashes…)`.
Constant lives in `consensus::kEpochSeedLookback` (not chain config; see
[BLOCK_PIPELINE.md — Protocol constants](../architecture/BLOCK_PIPELINE.md#protocol-constants-fork-critical-if-changed)).
If epoch `E-1` produced no blocks:
`SHA-256("pp-ledger/epoch-seed/empty-prev/v1" \|\| (E-1) \|\| prevEpochSeed)`.
Seeds chain through any run of empty epochs this way, each from the one before (an empty
epoch's stake snapshot is the bank after the last block), so a chain survives idle epochs — up
to **`kMaxEmptyEpochs` (1024)** in a row after the last block's epoch; past that no seed is
derived and the chain stops.

Validators reject blocks whose `epochSeed` ≠ the locally derived/installed seed
for that epoch.

## Leader election (live)

Implemented in `consensus::SlotCommittee` (beacon-centered schedule; **not**
classic Ouroboros / stake-weighted VRF on blocks):

1. Stakeholders = **registered miners** ([Miner registration and liveness](#miner-registration-and-liveness)):
   non-system accounts (`!isSystemAccount`, id ≥ `1<<20`) with a positive native balance of at
   least `minerMinStake`. Reserve, registrar, fee and recycle hold protocol funds and never
   lead; a new chain's committee is its genesis miners (registered at block 0).
2. Eligible **committee** = all if ≤`kMaxLeaderPoolSize` (**100**), else top
   **100** by stake (id tie-break). Constant:
   `SlotCommittee::kMaxLeaderPoolSize` — not chain config; parameterizing later
   requires genesis config (fork-critical). See
   [BLOCK_PIPELINE.md — Protocol constants](../architecture/BLOCK_PIPELINE.md#protocol-constants-fork-critical-if-changed).
   Stake gates **entry** into the committee only.
3. Require `epochSeed` for the slot’s epoch (forced leaders in tests bypass).
4. Leader = committee member at index from the first 8 bytes (BE) of
   `SHA-256("pp-ledger/slot-committee/v2" \|\| u64be(slot) \|\| u64be(epoch) \|\| epochSeed)`
   modulo pool size (**equal weight** within the committee — **designed
   behavior**, not a temporary stand-in for stake-proportional sampling).

Blocks commit `epoch` + `stakeSnapshotHash` + `epochSeed` so verifiers can check
the election inputs.

**Snapshot point (fork-critical):** an epoch's stake snapshot and seed are
taken from the chain as it stands just before that epoch's **first block** —
after every block of earlier epochs. They never depend on when a node's clock
crossed the epoch boundary: a node may elect leaders with a provisional
snapshot/seed ahead of that block, but re-derives both at the first block, and
a late block from an earlier epoch drops anything recorded for later epochs. Demo VRF / `EpochNonce` under `SlotLeaderSelection` are
**not** on the live `Chain` path. Ouroboros is a literature reference only.

**Domain:** `slot-committee/v2` (binary domain + seed). Block `CURRENT_VERSION` is **5**
(carries `epochSeed`).

## Slot timing

A slot's leader **collects for the whole slot and seals once it has ended**; the block must reach
the beacon within the **delivery window** after the slot's end.

- **Window** `D = min(slotDuration / 3, 2 s)` (`SlotCommittee::deliveryWindowMs`), fixed in code.
  Always shorter than a slot, so the next leader — who seals at the end of its own slot, after
  this slot's deadline — always seals on a settled tip: block *s*, or nothing for *s*. A leader
  never builds past a block still in flight.
- **Timestamp** (structural, every node, replay included): a block for slot *s* carries its seal
  time, `slotEnd(s) ≤ timestamp` and `timestamp·1000 < slotEnd(s)·1000 + D` (whole seconds,
  rounded down). A timestamp inside the slot is refused.
- **Delivery deadline** (beacon only, live admission): `BLOCK_ADD` for slot *s* is refused once the
  beacon's clock passes `slotEnd(s) + D` ("arrived after its delivery deadline"); the slot is then
  empty. Never checked on replay, late join or by relays: every node follows the beacon's chain.
  An already-held block is still answered as such (idempotent resend).
- **Leader** (`MinerServer::handleSlotLeaderRole`): pulls the beacon's pool one pull wait
  (`min(1 s, slot / 3)`) before its slot ends, then seals in `[slotEnd, slotEnd + D)`. Its pending
  transactions are validated against the slot the block will carry
  (`SlotCommittee::getProductionSlot`: the previous slot while inside its window).
- **Clocks:** miners take the beacon's clock offset (`CALIBRATION`); one skewed by more than `D`
  misses its slots rather than splitting the chain.

Open follow-ups (beacon failover, miner bonds, private leader selection):
[architecture/SLOT_COMMITTEE_OPEN_ITEMS.md](../architecture/SLOT_COMMITTEE_OPEN_ITEMS.md).

## Empty heartbeat blocks

**Settled policy:** heartbeat proves the chain is **active**. Any tip-advancing
work counts; the lag rule applies only to **empty** bodies.

When a slot leader has **no** renewals and **no** pending txs, they may still
seal a normal block with empty `records` if:

`currentSlot - tip.slot >= BlockChainConfig.heartbeatSlots`

(`heartbeatSlots == 0` disables). Default at beacon `--init` when the field is
omitted: `heartbeatSlots = slotsPerEpoch` (about one empty seal per idle epoch).

No special record type; fees are zero for the empty body. A **due miner**'s empty
block is admitted whatever the lag ([Miner registration and liveness](#miner-registration-and-liveness)).
**Validators enforce** the lag threshold on other empty bodies via `checkBlockBodyPolicy` (seal pre-apply +
`Full` `checkBlock`); premature empties and all empties when
`heartbeatSlots == 0` are rejected. Non-empty bodies (including renewals) are
**not** subject to the lag rule — they already demonstrate liveness. Leader,
time window, and header commitments still apply. Policy helper:
`shouldSealEmptyHeartbeat` / `validateEmptyHeartbeatPolicy`; miner UX gate in
`Miner::shouldAttemptProduction`. Pipeline:
[BLOCK_PIPELINE.md](../architecture/BLOCK_PIPELINE.md)
(Empty heartbeat settled note + Late join Full gate).

## Miner registration and liveness

Only **registered miners** are elected ([Leader election](#leader-election-live)). Registration is
on-chain account state (`Wallet.miner`, `UserAccount` version **2**): `registered`,
`lastProducedSlot`, `missedDue`, `autoDeregisteredEpoch`. Account creation and update cannot
set it (a new account carries the default state; an update or renewal keeps it); only the rules
below change it, in place — the account's `blockId` (where renewal reads its meta) does not move.

| Chain config | Beacon `--init` default | Meaning |
|--------------|-------------------------|---------|
| `minerMinStake` | 0 | Native balance a miner needs to register (after the fee) and to be elected |
| `minerRegistrationFee` | 0 | Flat fee `T_MINER_REGISTER` pays on top of the meta-size fee |
| `minerDueAfterSlots` | one day of slots | Slots after its last block (or registration) a miner is **due**; `0` = never, else ≥ 12 h of slots |
| `minerMaxMissedSlots` | 3 | Due misses in a row that deregister a miner; `0` = never |
| `minerReregisterCooldownEpochs` | 0 | Epochs after an automatic deregistration before it may register again |

All five are chain config: a `T_CONFIG` changes them at the next epoch boundary.

**Transactions** (`TxMiner`: `walletId`, `fee`, `meta`, `idempotentId` (non-zero),
`validationTsMin`, `validationTsMax`), signed by the account's own keys:

- `T_MINER_REGISTER` — refused for a system account, an account already registered, one in its
  cool-down (`epoch − autoDeregisteredEpoch < minerReregisterCooldownEpochs`), or one whose
  native balance after the fee is below `minerMinStake`. Fee ≥ meta-size fee +
  `minerRegistrationFee`, to the fee account. Sets `registered`, `lastProducedSlot` = the
  block's slot (a new miner is not due at once), `missedDue = 0`, and clears
  `autoDeregisteredEpoch`.
- `T_MINER_REVOKE` — the account must be registered; meta-size fee. Clears `registered`.

Either takes effect in the committee at the next epoch's stake snapshot (its first block).
Genesis miners are registered at block 0. There is no slashing: the cool-down is the penalty.

**Liveness.** A registered miner is **due** at slot `s` when `minerDueAfterSlots > 0` and
`s − lastProducedSlot ≥ minerDueAfterSlots`. On an idle chain a missing block proves nothing,
so the obligation is explicit: a due miner must produce in a slot it leads — an empty block if
it has no work — and that empty block is admitted whatever the tip lag (the producer's
on-chain state before the block decides). The leader schedule is unchanged.

After a block's records apply (fork-critical order, before `stateRoot`), every node:

1. for each slot the block skipped **in its own epoch** (after the previous block's slot), takes
   the slot's elected leader; if it was due at that slot, `missedDue += 1`; at
   `minerMaxMissedSlots` the miner is deregistered (`registered = false`, `missedDue = 0`,
   `autoDeregisteredEpoch` = the block's epoch) and leaves the committee at the next snapshot;
2. if the producer is registered, sets its `lastProducedSlot` to the block's slot and
   `missedDue = 0`.

Skipped slots of an earlier epoch are not charged. Code: `MinerTxHandler`, `MinerLiveness`
(`isMinerDue`, `applyMinerLiveness`); `pp-client miner-register` / `miner-revoke`; pp-miner
warns when its account is not a registered miner.

## Miner sign-in (`REGISTER` request)

Network-level: a miner announces itself to its upstream. It does not make the account a miner
on chain ([Miner registration and liveness](#miner-registration-and-liveness)).

`MinerInfo` (Meta, binary-packed in the request): `id`, `issuedAt` (signer's Unix seconds),
`signatures` (`binaryPack(vector<string>)`), and `tLastMessage` (set by the
beacon when recorded).

Each signature is the miner account key's ML-DSA signature over:

```
"pp-ledger/miner-register/v2" || pack(networkId) || pack(id) || pack(issuedAt)
```

`networkId` is the one the upstream advertises in `STATUS`. Accepted only if the
signatures satisfy the account's `publicKeys` / `minSignatures` (not a system
account), `|now − issuedAt| ≤ 5 min`, and
`issuedAt` is newer than the recorded registration.

## ChainNode / storage envelope

```text
ChainNode { block, hash }  // hash = SHA-256(header)
RawBlock  { data = Block::ltsToString(), hash }
```

Persisted via `VolumeStore` / `FileDirStore` / `FileStore` — see [LEDGER_STORAGE.md](LEDGER_STORAGE.md).
