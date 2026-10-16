# Beacon authority

How a node tells the real beacon's chain from any other, through any number of relays: what the
beacon signs, who certifies its key, how nodes adopt blocks, and how the authority changes hands.
Normative; rationale and history in [projects/beacon-authority](../../projects/beacon-authority/DESIGN.md)
([DECISIONS.md](../../projects/beacon-authority/DECISIONS.md)). Encoding conventions, records and the
genesis meta are in [WIRE_SCHEMA.md](WIRE_SCHEMA.md); the node files named here in
[LEDGER_STORAGE.md § Node state files](LEDGER_STORAGE.md#node-state-files).

## Trust chain

```text
genesis block (T_GENESIS, signed by the genesis quorum)
 ├─ recovery set ──── T_RECOVERY ───► replaces the admin set (announced, applied delayEpochs later)
 ├─ admin set (genesis account) ── T_CONFIG ──► replaces itself
 │                               └─ T_BEACON_CERT ──► beacon generation N+1 (its signing key)
 └─ beacon generation 1 (its signing key)
                                   beacon key ──► signed tip ──► every block below it (hash chain)
```

A node checks a signed tip by following this chain from what it already holds — never on an
upstream's word. Peer ids pinned in configs remain a convenience, not the trust anchor.

## Formats

Versioned forms start with their `uint32` version and are refused at any other version; there is
no migration (networks start from a new genesis on a change).

| Form | Version | Fields, in order | Carried in |
|------|---------|------------------|-----------|
| `ChainAuthority` | — (genesis meta v9) | `recovery {publicKeys, minSignatures, delayEpochs}`, `beacon` (a `BeaconCertificate`, unversioned), `hasPendingRecovery`, `pendingRecovery {announcement, announcedAt, applyEpoch}` | `GenesisAccountMeta.authority` ([WIRE_SCHEMA.md § Config updates](WIRE_SCHEMA.md#config-updates)) |
| `BeaconCertificate` | 1 | `networkId`, `generation`, `publicKey`, `cutoverHeight` | `T_BEACON_CERT` `meta` |
| `RecoveryAnnouncement` | 1 | `networkId`, `adminPublicKeys`, `adminMinSignatures` (`uint8`) | `T_RECOVERY` `meta` |
| `SignedTip` | 1 | `networkId`, `generation`, `height`, `blockHash` (raw), `signature` | `STATUS.signedTip`, `data/signed-tip.dat` |
| `TipReply` | — | `nextBlockId`, `hasSignedTip`, `signedTip` (unversioned) | `BLOCK_WAIT`, `BLOCK_ADD` replies |
| `Equivocation` | 1 | `first`, `second` (unversioned tips, `first.blockHash < second.blockHash`) | `STATUS.equivocation`, `EQUIVOCATION_ADD`, `data/equivocation.dat` |
| `AuthorityProof` | — | `steps`: `AuthorityStep {blockId, epoch, record}`… | `AUTHORITY_PROOF` reply |
| `RecoveryStatus` | — | `announcedAt`, `applyEpoch`, `adminPublicKeys`, `adminMinSignatures` | `STATUS.recovery` |

Code: `src/chain/Types.h` (authority, certificate, announcement), `src/client/Client.h` (tip,
evidence, replies).

## Genesis authority

Set by `T_GENESIS` (so signed by the genesis quorum) and carried unchanged by every later genesis
meta, except as the records below change it.

| Field | Meaning |
|-------|---------|
| `recovery.publicKeys` | Raw ML-DSA-65 keys of the recovery set: distinct, none an admin (genesis account) key |
| `recovery.minSignatures` | 1 ≤ m ≤ key count |
| `recovery.delayEpochs` | ≥ 1; epochs between announcing and applying a recovery |
| `hasPendingRecovery`, `pendingRecovery` | A recovery announced and not applied yet (below); none at genesis |
| `beacon.networkId` | The chain's `networkId` |
| `beacon.generation` | 1 at genesis |
| `beacon.publicKey` | Raw ML-DSA-65 beacon signing key: neither an admin nor a recovery key |
| `beacon.cutoverHeight` | 0 at genesis |

Code: `ChainAuthority`, checks in `chain_tx::validateGenesisAuthority`. `pp-beacon --init`
generates the generation-1 key (`keys/beacon-signing.key`, `.pub`) and certifies it in the
genesis record.

## Later generations — `T_BEACON_CERT` (record 18)

The admin set (genesis account, its `minSignatures`) certifies the next beacon. The record's `meta`
is a `BeaconCertificate` (version `1`, then `networkId`, `generation`, `publicKey`,
`cutoverHeight`); fee `0`; when `idempotentId` is non-zero (`mk-beacon-cert` sets one), an
idempotency key (genesis account, `idempotentId`) with the usual validation window. Rules (`chain_tx::validateNextBeaconCertificate`):

- `networkId` is the chain's; `generation` is exactly the one in force + 1;
- `publicKey` is a well-formed ML-DSA-65 key that is not the current beacon key, an admin key or a
  recovery key;
- `cutoverHeight` (the last height the previous generation may sign) does not go back, and the
  block that commits the record is at most `cutoverHeight + 1`: the previous generation never
  signs past it. A cutover not reached yet bounds nothing on chain; `2^64−1` (`mk-beacon-cert`
  without `--cutover`) is none — the old key signs until the certificate commits. In force, the
  certificate's `cutoverHeight` is the block before the one that commits it: the last block the
  previous generation signed (`chain_tx::certificateInForce`);
- not in the genesis block (that block certifies generation 1);
- **at most one per block** — a block holding two is refused whole (`validateAuthorityRecordCount`;
  `AuthorityTrail` likewise). The certificate is in force as soon as its block applies; a genesis
  meta (renewal, `T_CONFIG`) from then on carries it.

Out of band (a certificate a node receives outside a block), `Chain::verifyBeaconCertificateRecord`
checks the same record against the authority the node has: any generation above the one in force
passes, so a node that is behind can still recognise a newer beacon.

## Recovery — `T_RECOVERY` (record 19)

The recovery set replaces the admin set, announced on chain and applied `delayEpochs` later. The
record's `meta` is a `RecoveryAnnouncement` (version `1`, then `networkId`, `adminPublicKeys`,
`adminMinSignatures`); fee `0`; idempotency key (genesis account, `idempotentId`, non-zero) with the
usual validation window. It is **signed by the recovery set** in the authority —
`recovery.minSignatures` of its keys — not by any account. Rules
(`chain_tx::validateRecoveryAnnouncement`):

- `networkId` is the chain's; no recovery is pending already; at most one per block (a block
  holding two is refused whole); not in the genesis block;
- the new admin set has the genesis shape — 3 to 32 distinct well-formed ML-DSA-65 keys,
  2 ≤ `adminMinSignatures` ≤ key count — and none of its keys is a recovery key or the beacon key.

Committed in a block of epoch E, it is put in the authority as pending: `announcedAt` = that
block, `applyEpoch` = E + `delayEpochs`. Genesis metas carry it from then on, so a node starting
at a checkpoint sees it too.

- **Freeze:** while it is pending, the admin set issues nothing that changes the authority or the
  config — a `T_CONFIG` or a `T_BEACON_CERT` in a block of an epoch before `applyEpoch` is refused,
  and out of band every certificate is refused (one already held is dropped when the announcement
  commits). The admin set cannot cancel the recovery.
- **Apply:** the first block whose epoch is at or after `applyEpoch` replaces the genesis account's
  keys and threshold with the new set **before its records apply**, in place (the account's
  `blockId`, where renewal reads its meta, does not move), and clears the pending recovery. A
  certificate held out of band is checked again (the admins who signed it are gone).
- **Visible:** the beacon's and relays' `STATUS` carries `recovery` (`announcedAt`, `applyEpoch`,
  the new keys and threshold; absent if none) and `pp-client -b status` prints it; every node logs an error-level alert when the
  announcement commits, at each new epoch while it is pending, and when it applies.
- **Following it** (`AuthorityTrail`): sync and `AUTHORITY_PROOF` replays verify the announcement
  against the recovery set, apply the freeze, and replace the admin set at the apply epoch. Each
  authority step carries its block's epoch (`AuthorityStep {blockId, epoch, record}`); a replay up
  to a block takes that block's epoch too.

Tooling: `pp-client mk-recovery --network-id … --admin-pubkey … ×N --min-signatures M -o file`,
then `sign-tx` by each recovery key holder, then `submit-tx` to a relay or the beacon.

## Signed tip

The beacon signs **every block it commits** with its signing key, after the block is durably
stored, and keeps only the latest signature (`Client::SignedTip`):

| Field | Meaning |
|-------|---------|
| `networkId` | The chain's |
| `generation` | The beacon certificate in force for that block |
| `height` | Block index |
| `blockHash` | Raw block hash |
| `signature` | ML-DSA-65 by the certificate's key over `"pp-ledger/signed-tip/v1"` ‖ pack(`networkId`) ‖ pack(`generation`) ‖ pack(`height`) ‖ pack(`blockHash`) |

One signature authenticates the hash chain below it, so the block format does not change.

- **Never two tips at one height.** The beacon commits the block durably, then writes the tip to
  `data/signed-tip.dat` (version `1`, synced to the device) before serving it
  ([LEDGER_STORAGE.md § Durability](LEDGER_STORAGE.md#durability)), and never signs a different block at or below the height stored there.
  At start it refuses a ledger that ends below that tip or holds another block at its height
  (restore the ledger it signed), and signs the committed tip if it is not signed yet (genesis
  after `--init`, or a crash between commit and signing).
- **Only the certified key signs.** The beacon holds `keys/beacon-signing.key` and, staged for a
  rotation, `keys/beacon-signing.next.key`; it signs with whichever the certificate in force names.
  If it names neither — or a later certificate it holds out of band names another key — that
  generation belongs to another beacon: this one **stands down**. It refuses `BLOCK_ADD`,
  `TX_ADD` (except a beacon certificate, which it keeps but does not pool), `TX_PULL` and
  `REGISTER`; it serves reads and still takes `EQUIVOCATION_ADD`.
- **Where it travels:** the beacon's and relays' `STATUS` (`signedTip`, the packed versioned tip;
  absent before the first; beside it `certificate`, see [Certificates out of band](#certificates-out-of-band),
  and `equivocation`, see [Equivocation halt](#equivocation-halt)), and the `BLOCK_WAIT` and
  `BLOCK_ADD` replies, which are `TipReply {nextBlockId, hasSignedTip, signedTip}`. A `BLOCK_ADD`
  reply for a new block carries the tip that covers it; for a block the node already holds, the
  latest tip it holds (which may be higher). Relays keep the newest tip they hear from upstream
  that verifies (higher height, or higher generation at one height) and pass it on unchanged. A
  miner's `STATUS` is its own `MinerStatus` and carries none of these.

## Adoption rule

Relays and miners adopt blocks under this rule. A tip *verifies* when it names this chain's
`networkId` and the beacon generation in force at its height, and its signature checks with that
generation's key. The node finds that generation by following the blocks up to the tip from its own
state (`AuthorityTrail`): a `T_CONFIG` replaces the admin set, a `T_BEACON_CERT` raises the
generation, each only if the admin set in force at that point signed it (for a certificate, under
the on-chain rules above), and a `T_RECOVERY` signed by the recovery set freezes the admin set and
later replaces it. A tip of an older generation is superseded — once a certificate for generation
N+1 is in force, generation N signs nothing above it (in force, its `cutoverHeight` is the block
before the certifying one); a later generation no block certifies does not verify. Nor does a tip
above the cutover of a later certificate the node holds out of band, or a tip of a generation it
holds equivocation evidence for, above that evidence's height.

- **Sync** (`BlockSync`) commits only blocks a verified tip vouches for: it takes the upstream's
  tip from `STATUS`, fetches the blocks from the local tip up to it, checks that each hashes to
  what it claims and extends the one below it up to the signed hash, and only then applies them.
  Blocks above the tip wait for the next tip. A local tip at the signed height with another hash
  fails the sync.
- **Own blocks:** a miner commits the block it produced only when a `BLOCK_ADD` reply carries a
  verified tip for exactly that block (height and hash); a relay forwarding a `BLOCK_ADD` stores
  the block under the same rule. Otherwise the block arrives later by sync, if the beacon took it.
- **Upstream choice:** a miner with several upstreams follows the one whose signed tip has the
  highest generation, then the longest chain; a false claim fails its sync and the miner rotates.
- **First block:** a node with no blocks authenticates its whole range from its first block —
  genesis (its `T_GENESIS`) or a checkpoint (its opening genesis renewal) — which gives the
  authority and admin set the tip must verify under; nothing commits before. Later genesis
  renewals must agree with the authority followed. That first block is the node's root of trust:
  - a **relay** starts at genesis and may pin it (`networkAnchor.genesisHash`);
  - a **miner** starts at the upstream's checkpoint. Pinning genesis, it fetches `AUTHORITY_PROOF`
    up to the checkpoint and replays it from genesis (below); the checkpoint's renewal must carry
    the authority and admin set the replay reaches, else the miner starts at genesis. The sync
    checks the block it actually starts from again.

  Without a pin the first block is taken on trust.

## Certificates out of band

A certificate is self-verifying (genesis → admin set → certificate), so it may reach a node before
any block commits it — the case of a stolen beacon key, whose holder will never commit its
successor. Every node keeps the highest one it sees:

- **Channels.** The beacon's and relays' `STATUS` carries `certificate` (the packed `T_BEACON_CERT`
  record): the one the node holds out of band, else the latest committed one in the blocks it holds
  (absent at generation 1, and on a node that started at a checkpoint above the certifying block).
  A `TX_ADD` of a `T_BEACON_CERT` is offered at every node it passes — relays and miners keep it and
  forward it, kept or not (when forwarding fails, the reply still says it was kept); the beacon
  keeps it and pools it (a beacon that stood down keeps it only), and refuses one that is not
  above the generation in force or held. An operator hands a node a certificate file with
  `pp-client submit-tx` to that node.
- **Kept** (`Chain::offerCertificate`): verified against the node's own committed state as above;
  kept if its generation is above both the one in force and the one held (another certificate for
  the generation held is refused with a warning: the first stays). The record is written to
  `data/beacon-cert.dat` before the offer returns and offered again at start. It is dropped, and
  its file removed, once a block commits its generation (or a later one), or when an admin
  rotation leaves it unsigned. A node with no blocks cannot verify it: it keeps a stored one until
  its first block and refuses offers until then, so **its first sync is not bounded by it**.
- **Effect**, until a block commits that generation: a tip of an older generation **above its
  cutover does not verify**, whatever key signed it — at sync (the upstream's `STATUS` certificate
  is offered before its tip is checked), on `BLOCK_ADD` replies, and for the tip a relay serves (a
  relay stops serving one the certificate rules out). A miner choosing its upstream at start skips
  one whose tip is past it; later, a sync to it fails and the miner moves to its next upstream. A
  beacon holding no key the certificate names stands down; one that holds it (a staged rotation)
  commits no block above the cutover that does not commit a certificate.

A tip of the new generation still verifies only along blocks that commit its certificate: the
certificate bounds the old generation, it does not let a node follow the new one before the chain
does (the new beacon commits it right after the cutover — part 2 of beacon-authority).

## Equivocation halt

Two signed tips of one network, generation and height that name **different blocks** prove the
generation's key signed twice — it is stolen, or two hosts run it. An honest beacon cannot produce
them (it never signs a second block at or below its stored height). Signatures alone may differ
for one block (ML-DSA signs with fresh randomness): only the block hash counts.

- **Evidence** (`Client::Equivocation`, version `1`): the two tips, ordered by block hash. It
  verifies when both tips are of one network, generation and height with different hashes, and
  both signatures check with that generation's key — the one in force, or the certificate held out
  of band. Evidence for an older generation is moot (superseded); for a generation no
  certificate here names, it is refused until the certificate arrives.
- **Halt:** a node holding evidence for generation G at height H verifies no tip of G above H
  (`AuthorityTrail`), so it commits nothing past H by any path (sync, own block, relay). Miners
  produce nothing above H (choosing an upstream at start, they skip one serving such tips); relays stop serving a held tip
  above H; the beacon commits no block above H and refuses `BLOCK_ADD`, `TX_PULL`, `REGISTER` and
  `TX_ADD` — except a beacon certificate, the way out (and it still takes `EQUIVOCATION_ADD`). Every node keeps serving reads, logs an
  error-level alert, and keeps the evidence in `data/equivocation.dat` (written before the offer
  returns; offered again at start). A node holds at most one piece: other evidence is ignored
  while it does. A node with no blocks keeps stored evidence until its first block and refuses
  offers until then, so its first sync is not halted by it.
- **Resume:** a certificate of a later generation — seen out of band or committed — supersedes G;
  the evidence is dropped and its file removed.
- **Where it travels:** down on `STATUS` (`equivocation`, the packed versioned evidence; absent if
  none), offered after `certificate` and before the tip; up by `EQUIVOCATION_ADD` (request 1005,
  payload the packed evidence, reply a short text; downstream to upstream only): relays keep it
  and forward it, kept or not; miners keep it and forward it to their active upstream; the beacon
  keeps it. A relay whose upstream sends a second verified tip for another block at one height
  builds the evidence and pushes it up; so does a miner whose upstreams serve such tips when it
  compares them (choosing its upstream at start), to every upstream — each side of a split runs
  its own beacon host. `pp-client add-equivocation <file>` submits evidence to any role;
  `pp-client mk-equivocation` signs drill evidence with a beacon key.

Only the latest tip is kept, so evidence is found when two tips of one height meet at a node; a
conflicting tip below a node's latest goes unnoticed.

## Authority proof

`AUTHORITY_PROOF` (request 1004, payload `uint64 upToBlockId`, reply the packed `AuthorityProof`;
low-priority lane). The records that changed the authority (`T_CONFIG`, `T_BEACON_CERT`,
`T_RECOVERY`) from genesis up to that block, each with its block id and epoch, in chain order
(`Client::AuthorityProof`). Served by nodes holding the chain from genesis (beacon, relays); refused
by a node that started at a checkpoint, and for a block at or beyond the node's tip. Miners do not
serve it. A node replays it from a genesis block it trusts (`AuthorityTrail::replayProof`), taking
each step only if the admin set in force there signed it: an upstream can withhold or reorder steps
(the replay then misses the starting block's authority or fails), not forge them.

## RPC

What the beacon authority adds to the ledger RPC ([LEDGER_TOPOLOGY.md § Realization contract](../architecture/LEDGER_TOPOLOGY.md#realization-contract)):

| Request (id) | Change |
|--------------|--------|
| `STATUS` (1), beacon and relays | Meta keys, each absent when there is none: `signedTip` (`SignedTip`, versioned bytes), `certificate` (packed `T_BEACON_CERT` record), `equivocation` (`Equivocation`, versioned bytes), `recovery` (packed `RecoveryStatus`). A syncing node offers `certificate`, then `equivocation`, then checks `signedTip` |
| `BLOCK_ADD` (1002), `BLOCK_WAIT` (1003) | Reply payload is a packed `TipReply` |
| `AUTHORITY_PROOF` (1004) | New: [Authority proof](#authority-proof). Answered by nodes holding the chain from genesis |
| `EQUIVOCATION_ADD` (1005) | New: payload `Equivocation` (versioned bytes), reply a short text; downstream to upstream only ([Equivocation halt](#equivocation-halt)) |
| `TX_ADD` (3002) | A `T_BEACON_CERT` is also kept by every node it passes ([Certificates out of band](#certificates-out-of-band)) |

A beacon that stood down, or holds equivocation evidence, refuses `BLOCK_ADD`, `TX_PULL`,
`REGISTER` and every `TX_ADD` but a beacon certificate; it serves every read and takes
`EQUIVOCATION_ADD`.
