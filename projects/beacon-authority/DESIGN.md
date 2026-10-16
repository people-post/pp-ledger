# Beacon authority — design

**Status:** part 1 shipped (B001–B021; normative in [BEACON_AUTHORITY.md](../../docs/contracts/BEACON_AUTHORITY.md)); part 2: cases 2 and 3 shipped (bp1–bp4), case 1 (re-genesis) designed (bp5)
**Open item:** [SLOT_COMMITTEE_OPEN_ITEMS.md § E](../../docs/architecture/SLOT_COMMITTEE_OPEN_ITEMS.md)
**Decisions:** [DECISIONS.md](DECISIONS.md) · **Status today:** [CURRENT_STATE.md](CURRENT_STATE.md)

The network has one active beacon. It is the only node that commits blocks, and every other node
follows its chain. This project makes that authority **verifiable** (part 1) and **transferable**
(part 2):

1. **Part 1 — the real beacon.** A miner or relay can tell the real beacon's chain from a chain
   served by any other node, through any number of relays.
2. **Part 2 — promotion.** Another node can become the beacon after a loss or a planned move,
   and the old beacon cannot come back as a second authority.

Part 1 lands first: part 2 is "issue a new authority under part 1's rules".

---

## Where we started

Before part 1 (the first three rows are closed by it; the last two are part 2's):

| Before | Consequence |
|-------|-------------|
| Nodes pin their upstream's Amp peer id in config; the Amp handshake proves the peer holds that key | Authenticates **one hop** only. Miners usually dial relays, so nobody checks the beacon behind them |
| Blocks are not signed (header hash only; `slotLeader` is checked against the election) | Anyone with the genesis can build a valid-looking chain — e.g. of empty heartbeat blocks — and serve it |
| Nothing in chain data names the beacon | A promoted beacon needs no chain change to be valid — but nothing stops two beacons either |
| No fork choice: a node only extends its own tip; the ledger can rewind, nothing calls it | Two active beacons split the network for good; nodes on the losing side are stuck |
| The delivery deadline is judged on the beacon's clock and not recorded | Two beacons can disagree on a slot with no way to reconcile |

The genesis **admin set** — the genesis account's keys and `minSignatures` (2 of 3 today) — is the
existing root of trust: it signs genesis and `T_CONFIG`, and `T_CONFIG` can replace the genesis
account's keys at once ([WIRE_SCHEMA.md](../../docs/contracts/WIRE_SCHEMA.md)).

---

## Part 1 — the real beacon

Shipped. This part keeps the design as decided; where it and the contracts disagree, the
contracts win ([BEACON_AUTHORITY.md](../../docs/contracts/BEACON_AUTHORITY.md)).

### Trust roots (in genesis)

| Set | Holds | Powers | Changed by |
|-----|-------|--------|------------|
| **Recovery set** (new) | Public keys + threshold (e.g. 3 of 5), held apart, used almost never | **Only** replace the admin set — announced, applied ~30 days later ([B004](DECISIONS.md#b004--recovery-set-narrow-slow-visible), [B007](DECISIONS.md#b007--recovery-delay-about-a-month-admin-set-frozen-meanwhile)) | Itself (its own threshold) |
| **Admin set** (existing genesis account) | Public keys + `minSignatures` | Genesis, `T_CONFIG`, **issue beacon certificates** | Itself (`T_CONFIG`, as today) or the recovery set |
| **Beacon signing key** (new) | One ML-DSA key on the beacon host, `keys/beacon-signing.key` + `.pub` — separate from the Amp identity ([B006](DECISIONS.md#b006--a-separate-beacon-signing-key)) | Sign tips for its generation | A new certificate from the admin set |

The recovery set is the top authority, so it must come from genesis and must not be changeable by
`T_CONFIG` (like `genesisTime`): if the admin set could set it, the admin set would be on top again.

### Beacon certificate

```
BeaconCertificate {
  networkId
  generation        // 1 at genesis; +1 per promotion
  publicKey         // ML-DSA beacon signing key
  cutoverHeight     // generation > 1: last height the previous generation may sign
}
// Signed by the admin set through the record that carries it (generation 1: the genesis record).
```

- **Generation 1** is issued at `pp-beacon --init`: it generates the beacon signing key, and the
  genesis keys (already present for genesis) sign the certificate over its public key — one
  ceremony.
- The certificate binds the **signing key**, not the Amp peer id. It may carry the beacon's current
  peer id as an **unsigned dial hint**; nothing trusts it. Rotating the Amp identity therefore needs
  no admin session (downstream configs re-pin it); rotating the signing key needs a new generation.
- Certificates are **self-verifying** (genesis → admin set → certificate), so they may arrive by
  any channel: on chain, from any peer, or a file an operator hands to a node.
- On chain, a certificate is a record signed by the admin set (new record type), so it reaches every
  node through normal sync and survives restarts.
- **Only the admin set raises the generation** ([B002](DECISIONS.md#b002--only-the-admin-set-raises-the-generation)).
  A beacon cannot delegate to a successor: a stolen beacon key could otherwise hand itself a
  higher generation.

### Signed tip

The beacon signs **every block** it commits ([B009](DECISIONS.md#b009--sign-every-block-keep-only-the-latest-signed-tip)):

```
SignedTip { networkId, generation, height, blockHash, signature /* beacon key */ }
```

- **Sign after durable commit**, and persist the last signed height. The beacon never signs a
  different block at or below it — so a crash between signing and storing cannot make an honest
  beacon sign two tips at one height.
- Served on `STATUS`, `BLOCK_WAIT` and the `BLOCK_ADD` reply. **Relays pass it through unchanged.**
- **Only the latest signed tip is kept** (plus any equivocation evidence), on the beacon and on
  every node — not one signature per block (~3.3 KB each). The hash chain proves everything below
  it; what is lost is proof that the beacon signed a particular *old* block.
- **Adoption rule:** a node commits synced blocks only up to a tip signed by the current beacon,
  after checking the hash chain from its local tip to that tip. One signature authenticates every
  block below it, so the block format does not change.
- A miner commits its own block only when the `BLOCK_ADD` reply carries the beacon's signed tip
  for it (today it commits on the first upstream success, even if the block reached no beacon).

A fake upstream can still **withhold** or serve **stale** data. It cannot forge a tip. Pinned peer
ids stay as a convenience, not as the trust anchor.

### Generations

- A node follows the highest valid generation it has seen.
- Generation N signatures stay valid up to generation N+1's `cutoverHeight`; any generation N tip
  above it is rejected. On chain, N+1 therefore commits by the block after the cutover, and the
  cutover in force is the block before its commit; a certificate with no cutover (an honest
  rotation) bounds nothing until it commits ([B017](DECISIONS.md#b017--certificates-out-of-band-bound-the-old-generation-the-cutover-bounds-the-commit)).
- A beacon that sees a higher generation than its own stops committing and runs as a relay.

### Equivocation halt

Two different signed tips with the **same generation and height** are proof that the beacon key is
stolen or that two hosts run it ([B005](DECISIONS.md#b005--equivocation-halts-nodes)). With
sign-after-commit an honest beacon cannot produce them.

On seeing them, a node:

- stops committing and producing blocks above that height, keeps serving reads;
- alerts the operator and passes the evidence (both signed tips) to its peers, so the network
  halts together;
- resumes on its own once a valid certificate of a higher generation arrives.

This adds no new denial of service: producing the evidence takes the beacon's key.
Shipped in ba6 ([B018](DECISIONS.md#b018--equivocation-evidence-by-block-hash-halt-at-the-tip-gate-resume-on-a-later-generation)).

### Recovery action

- The recovery set signs **announce(new admin set)**; the record lands on chain and is shown
  prominently: on `STATUS`, in node logs, and as an operator alert. A month nobody sees is no
  safeguard.
- It **applies after a fixed number of epochs** (~30 days of slots, set in genesis) — counted on
  chain, so every node applies it at the same block.
- **While a recovery is pending the admin set is frozen:** no new beacon generations and no
  `T_CONFIG` ([B007](DECISIONS.md#b007--recovery-delay-about-a-month-admin-set-frozen-meanwhile)).
  A pending recovery means the admin set is suspect; stolen admin keys get no month of power.
- The admin set cannot cancel it ([B004](DECISIONS.md#b004--recovery-set-narrow-slow-visible)). If
  the recovery set itself is stolen, the month is for people to act out of band.

Shipped in ba7 ([B019](DECISIONS.md#b019--recovery-a-record-of-its-own-pending-in-the-authority-applied-in-place-at-its-epoch)):
the effect needs no record of its own — the first block of the apply epoch replaces the admin set.

### Wire and storage (shipped)

Promoted in ba8; the normative copies, so this section lists no formats:

- [BEACON_AUTHORITY.md](../../docs/contracts/BEACON_AUTHORITY.md) — formats and versions, records
  18 / 19, signed tip, adoption rule, certificates out of band, equivocation halt, authority proof,
  RPC changes.
- [WIRE_SCHEMA.md](../../docs/contracts/WIRE_SCHEMA.md) — record ids, the genesis meta (v9) that
  carries the authority, checkpoints.
- [LEDGER_STORAGE.md § Node state files](../../docs/contracts/LEDGER_STORAGE.md#node-state-files) —
  signing keys, `signed-tip.dat`, `beacon-cert.dat`, `equivocation.dat`.
- [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md) — operator procedures: rotation, a stolen
  key, replacing the admin keys.

### Settled questions

| Question | Answer |
|----------|--------|
| Beacon signing key vs Amp identity | Separate key ([B006](DECISIONS.md#b006--a-separate-beacon-signing-key)) |
| Recovery delay | ~30 days, in epochs; admin set frozen meanwhile ([B007](DECISIONS.md#b007--recovery-delay-about-a-month-admin-set-frozen-meanwhile)) |
| Existing networks | None released: breaking change, new genesis ([B008](DECISIONS.md#b008--breaking-genesis-change-no-migration)) |
| Signing cadence | Every block; keep only the latest signed tip ([B009](DECISIONS.md#b009--sign-every-block-keep-only-the-latest-signed-tip)) |

---

## Part 2 — promotion

**Status:** bp0 design note, decided: [B022](DECISIONS.md#b022--a-rewind-is-a-restart),
[B023](DECISIONS.md#b023--every-node-adopts-a-cutover-on-its-own),
[B024](DECISIONS.md#b024--case-2-certificate-v2-a-promoted-relay-no-outage-charges).

All three cases issue or re-use a certificate under part 1's rules; nodes follow the highest
generation.

| Case | Keys | What happens |
|------|------|--------------|
| **1. Everything lost** — beacon key and admin keys | none | No protocol path can recover (one would serve an attacker too). **Re-genesis:** new keys and a new genesis that commits to the last good state (block hash + state root at a chosen height), published and announced out of band. The recovery set makes this rarer: losing the admin keys alone is case 2-like. |
| **2. Beacon stolen** — beacon key taken, admin keys safe | admin set | The admin set issues generation N+1 to a caught-up node, with `cutoverHeight` = the last tip trusted before the theft. Nodes on the thief's branch rewind to it; transactions after it are resubmitted (idempotency keys make that safe). Cut-off nodes get the certificate by file or config. |
| **3. Move the server** — all keys safe | all | **Plain operations** ([B003](DECISIONS.md#b003--moving-the-server-is-plain-operations)): stop writes, copy data and beacon key, start the new host, retire the old. Same generation. Rotating to a new key (generation N+1 with a clean cutover) stays the operator's choice. |

### Principles

- **Rare events, simple mechanisms** ([B022](DECISIONS.md#b022--a-rewind-is-a-restart)). Part 2 runs
  perhaps once in a network's life. Prefer what reuses paths every node runs daily — mount, load
  from the ledger, startup sync, fresh join — over faster special-purpose code that runs once a
  year and then breaks. Slow is fine; a restart is fine.
- **Automatic for every node** ([B023](DECISIONS.md#b023--every-node-adopts-a-cutover-on-its-own)).
  Miners and relays the maintainer does not run adopt a cutover with no operator and no service
  supervisor.
- **No new authority.** Only an admin-signed certificate (part 1) moves anything: a stolen beacon
  key or a lying upstream cannot trigger a rewind or a takeover.

### Case 2, end to end

1. **Detect.** The equivocation halt (ba6) stops the network, or an operator finds the key stolen.
2. **Certify.** The admins choose the last block they trust, *C*, and a new beacon host — one of
   the maintainer's relays, which holds the chain from genesis — and sign generation N+1: the new
   host's signing key, `cutoverHeight` = *C*, `cutoverHash` = block *C*'s hash, and `dialHints` =
   gateways (the maintainer's relays) through which the new beacon is reached.
3. **Spread.** The certificate is submitted to the maintainer's relays; it travels down on
   `STATUS` (part 1) and **up** from every node that keeps it (bp2), so a third-party relay whose
   only upstream is the old beacon still learns it from its miners.
4. **Rewind.** Every node holding blocks above *C* rewinds to *C* by restarting itself (below).
5. **Take over.** The maintainer starts `pp-beacon --promote` on the chosen relay's data. It
   holds the certificate and its key, so it takes over at *C* instead of standing down.
6. **Resume.** Nodes reach the new beacon (candidates and hints, bp2). The next slot leader seals
   block *C*+1 carrying the certificate; the new beacon commits it and signs it as generation N+1.
   From there the network runs as before; the old key's chain is dead to every node.

### Rewind — decided ([B022](DECISIONS.md#b022--a-rewind-is-a-restart), [B023](DECISIONS.md#b023--every-node-adopts-a-cutover-on-its-own))

- **Trigger.** A node keeps a certificate of a later generation whose cutover *C* is below its
  committed tip (part 1 already refuses the old generation's tips above *C*). The blocks above *C*
  were signed by the old key past its cutover — invalid by the certificate's own terms — so the
  node needs nothing more to know they must go. A node whose block *C* is not `cutoverHash` halts
  and alerts instead (bp2): a fork at or below *C*, or a typo'd height, needs a human.
- **Marker.** It writes `data/rewind.dat` durably — target height and the certificate that orders
  it — logs an error-level alert, and stops.
- **Restart in-process.** The app's `main()` runs the role server in a loop: when `run()` returns
  "restart", it builds a **fresh** server on the same work directory. Every object — chain,
  stores, sync, pools, network — is rebuilt, exactly as at a cold start; the clean stop releases
  the port first. Audited global state: the libsodium initialiser (idempotent), the logger, and
  the app's signal-handler pointer — none carries chain state. No supervisor is needed; with one,
  nothing changes.
- **Apply at start.** Before anything mounts the ledger, the new server applies the marker:
  truncate to *C* (the commit point is lowered before the data is cut, so a crash at any step
  repeats the rewind at the next start), then remove the marker. If *C* is below a miner's
  starting checkpoint, it discards the ledger
  and joins fresh — a relay from genesis, a miner at a checkpoint — through the existing fresh-join
  path. Then it syncs as usual.
- **Bounded.** A node's blocks no longer pass the cutover after the rewind, so the order is not
  issued again; an order that cannot be applied stops the start with an error rather than looping.
  **Depth** ([B025](DECISIONS.md#b025--a-node-rewinds-on-its-own-only-seven-days-deep)): a node rewinds
  on its own only within seven days of slot time; deeper, it stops at its tip and waits for its
  operator's `--approve-rewind <generation>`. More guards come with certificate v2 (bp2): the
  cutover may not drop the block that committed the generation in force; a node whose block *C* is
  not `cutoverHash` halts and alerts instead of joining fresh (a typo'd height must not wipe more);
  `mk-beacon-cert` fills `cutoverHash` from a node and shows the depth before the admins sign.
- **Back online.** A node that was down during the cut learns the certificate from its upstream's
  `STATUS` in its startup sync and rewinds then, whether or not that sync succeeds.
- **Reported.** Clients that read blocks above *C* see history change: the node logs it and
  reports its last rewind on `STATUS` (shape in bp1). Transactions from those blocks must be
  resubmitted — idempotency keys make it safe; ones whose validity window passed must be re-signed.
- **Honest rotations never rewind.** No cutover, or a cutover still above the tip, triggers
  nothing. A cutover below the tip is a rewind order (B017); the operator docs must say so.

### Certificate v2 — `cutoverHash` and `dialHints`

`BeaconCertificate` version 2 adds, admin-signed with the rest:

- **`cutoverHash`** — block *C*'s hash; empty when there is no cutover. It makes the rewind target
  unambiguous, and it closes the **takeover gap**: once nodes refuse the old generation above *C*,
  no tip of any generation vouches for block *C* until *C*+1 is signed — a node behind *C* could
  not sync to it. With the hash, the certificate vouches for *C*: `BlockSync` may take
  (*C*, `cutoverHash`) from a held certificate as its target, authenticate the hash chain up to it,
  and commit with no signed tip. The new beacon gets a signed starting point the same way.
- **`dialHints`** — multiaddrs of **gateways** through which the new beacon is reached: the
  maintainer's relays in front of it, never the beacon itself, which stays private behind its relays
  (`allowedPeers`). Upstreams are opaque endpoints and relays may chain
  ([AMP_TRANSPORT.md § Topology](../../docs/contracts/AMP_TRANSPORT.md#topology-uniform-upstream)),
  so a third-party relay or miner adds a hint as one more upstream candidate. Only a hint: nodes
  trust tips, not addresses. The admins sign them with the certificate, so the hinted relays should
  be long-lived; a later change of front relays without a new generation reaches nodes through
  their candidate lists.

Without v2: the rewind target is a height only, and a node behind *C* waits for a tip nobody serves
any more — workable only if every node is at or above *C* when the certificate lands.

### Reaching the new beacon (bp2)

- **Certificates travel up.** A node that keeps a new certificate pushes it (`TX_ADD`) to every
  upstream, as miners already push equivocation evidence: a third-party relay behind the old beacon
  learns it from its miners. A subtree with no path to any node holding the certificate cannot
  learn it; its operators get the file out of band (part 1).
- **Candidate beacons.** Relays take a list of upstreams, as miners do, and follow the one whose
  verified tip has the highest generation. A held certificate's `dialHints` are added as candidates.
- **Re-rank** on a failed sync and on a new certificate, not only at start (part 1 limit: miners
  rank once).
- **Fresh nodes** (part 1 limit): a node with no blocks checks the upstream's `STATUS` certificate
  against the authority it builds from its first block (`AuthorityTrail`) before it checks the tip,
  so its first sync is bounded by the cutover too.

### Taking over (bp3)

- **Host and tooling.** The new beacon is one of the maintainer's relays: its work directory
  already holds the chain from genesis, the identity key, and the certificate it kept. Relay and
  beacon directories differ only in `config.json` and the beacon's signing key and
  `signed-tip.dat`. `pp-beacon --promote -d <relay dir>` checks the staged signing key against the
  held certificate, rewrites `config.json` for a beacon (keeping port and `allowedPeers`), and
  starts. The maintainer runs it — an operator step only on the new beacon's host — and points the
  hinted relays at it (their candidate lists).
- **Taking-over state.** A beacon holding a certificate above the one in force that names its own
  key does not stand down (part 1 stands it down because the in-force key is not its own). It
  serves reads and `STATUS` (certificate, no signed tip), commits only block *C*+1 carrying that
  certificate, and signs from *C*+1 on.
- **Liveness.** Block *C*+1 skips every slot since *C* — the outage, not the miners' fault. The
  block that commits a certificate with a cutover charges no skipped slots (otherwise due miners
  would be counted as missing for the whole outage and deregistered).
- **Pool and registry rebuild from scratch.** Miners re-register within a minute (`REGISTER`
  renewal); the transaction pool starts empty and clients resubmit.
- **Clock.** The beacon's clock is its host's (NTP); relays and miners calibrate to their upstream.
  At takeover the new beacon compares its clock with its relays' calibrated time and refuses to
  take over beyond a bound (a fraction of the delivery window); nodes recalibrate when they start
  following a new generation (rewound nodes do at their restart).

### Case 3 — move the server (bp4)

Plain operations (B003): stop writes, copy the data directory and the signing key, start on the new
host, retire the old. Same generation, same key; running both at once is caught by the equivocation
halt (B005). Relays reach the new host through their candidate lists. Shipped (bp4,
[B029](DECISIONS.md#b029--a-beacon-with-no-signed-tip-does-not-sign-a-ledger-past-genesis-unasked)):
the runbook [MOVING_THE_BEACON.md](../../docs/ops/MOVING_THE_BEACON.md), the drill `--suite move`,
and one guard — a copy without `data/signed-tip.dat` does not sign its ledger unasked.

### Case 1 — re-genesis (bp5)

**Status:** bp5 design note, decided: [B030](DECISIONS.md#b030--re-genesis-a-new-network-that-imports-the-old-state-checked-against-its-state-root),
[B031](DECISIONS.md#b031--the-state-root-commits-to-the-whole-account),
[B032](DECISIONS.md#b032--the-import-rides-in-import-blocks-after-genesis),
[B033](DECISIONS.md#b033--re-genesis-tooling-and-roles).

**When.** No authority is left that the protocol could follow: the beacon key and the admin keys
are lost or stolen, and the recovery set cannot act (lost too, or the admin set it would replace is
not the problem). If the recovery set can act, it replaces the admin set and case 2 follows; that is
always preferable — it keeps the network, its history and every signature. Re-genesis is the last
resort, and deliberately **out of band**: no record on the old chain can order it, because one that
could would serve a thief as well ([§ Principles](#principles): no new authority).

**What it is.** A **new network** — a new `networkId`, new genesis, admin, recovery and beacon keys —
whose genesis **imports the old network's account state at a chosen height *H***, verbatim, and
names where it came from:

```
predecessor { networkId, height H, blockHash, stateRoot }   // in the genesis meta (v11)
```

Every node of the new network checks the import against `predecessor.stateRoot` — the root the old
chain committed in block *H*'s header. The state root covers every account (id, balances, keys,
threshold, miner state and — from [B031](DECISIONS.md#b031--the-state-root-commits-to-the-whole-account)
— its attachment), so the import is exactly the old state at *H* or the new chain does not start.
What the people running the re-genesis choose is only *H*, the new keys and the new config: they
cannot move a balance, change a key or add an account. Whether block *H* is the old network's block
is checked by anyone holding the old chain (one header), and announced with the new genesis.

#### Choosing *H*

- **Keys lost, chain halted** (no beacon can sign): *H* is the last signed tip — every node has it.
- **Keys stolen:** as for a case 2 cutover, the last block everyone trusts — below anything the thief
  signed. Blocks above *H* are not carried; their transactions must be signed again for the new
  network (below).

#### The import

- **Records.** A new record type, `T_IMPORT` (20), carries one account as the old chain held it: its
  id, wallet, attachment, and the old `blockId` its leaf was hashed with. Unsigned: the state root
  authenticates it. `T_NEW_USER` cannot carry an account (one native balance only, no miner state,
  fees, the genesis supply rule).
- **Import blocks.** Block 0 is the genesis (`T_GENESIS` with the predecessor, no system records);
  blocks 1 … *k* — the **import span** — hold only `T_IMPORT` records, by ascending account id, at
  slot 0, as many as fit the RPC's 512 KiB per block (~200 one-key accounts). They reuse every path
  a node already has: sync, adoption up to a signed tip, storage. `pp-beacon --init --import-state`
  writes blocks 0 … *k* and signs the tip at *k*; the first produced block is *k* + 1.
- **The check.** Nodes fold each imported leaf, `(id, wallet, old blockId, attachment hash)`, into a
  tree as the span arrives; at block *k* its root must equal `predecessor.stateRoot`, and the span
  must end there (the last block says so). A node refuses an import block out of order, an id twice
  or out of order, or a span whose root does not match.
- **Applied as.** Each account enters the new state at its import block (that is its `blockId` now:
  renewals are due from there; its attachment is hydrated from the `T_IMPORT` record). Its old leaf
  is checked as imported; then fields that count slots or epochs of the old network are reset, since
  the new `genesisTime` gives them no meaning: a miner's `lastProducedSlot` = 0 (a fresh due period),
  `missedDue` = 0, `autoDeregisteredEpoch` = never; `registered` is kept.
- **System accounts** (genesis, reserve, registrar, fee, recycle) keep their balances; their keys
  come from the new `init-config.json` — the old ones are the lost ones. The new admin set is the
  genesis account's new keys; the recovery set and the beacon certificate (generation 1) are new.
- **Supply** is what was imported: the genesis rule "spend exactly the initial supply" does not apply
  to an importing genesis; balances move unchanged, so every token's total is the old one.
- **Committee.** Epoch 0's committee is the registered miners after the import (stake ≥
  `minerMinStake`), as a new chain's is its genesis miners. Miners keep their accounts and keys.
- **Config.** The old network's `BlockChainConfig` is carried except `networkId` (new, required to
  differ) and `genesisTime` (the new genesis); `init-config.json` may override any field.
- **Not carried:** the transaction history (the old chain stays the archive; the predecessor link
  joins them), the pool, out-of-band certificates and evidence, a pending recovery or config update,
  the issuance baseline (it restarts from the imported genesis balance).

#### Who does what

1. **The maintainer** (holder of the new keys) stops trusting the old network, picks *H*, and on a
   copy of a full node's directory (a relay holds the chain from genesis) runs
   `pp-client export-state -d <copy> --height H -o state.snap`: a versioned snapshot whose header is
   the predecessor and whose root it checks against block *H* before writing.
2. It writes a new `init-config.json` (new `networkId`, new system, recovery and beacon keys) and runs
   `pp-beacon --init --import-state state.snap`; then starts the beacon and its relays.
3. It **announces out of band** the new network id, genesis hash and gateways, with the predecessor
   (old network id, *H*, block hash). Anyone holding the old chain runs
   `pp-client check-predecessor -d <old ledger> --host <new relay>` to see that block *H* is theirs.
4. **Operators** of relays and miners start fresh nodes for the new network: new work directory, the
   new genesis pinned (`networkAnchor.genesisHash`), the new upstreams. Miners keep their keys and
   account ids; their registration and stake came across.
5. **Users and clients** switch network id and endpoints. Account ids, balances and keys are
   unchanged. Every signature binds the network id, so nothing signed for the old network is valid on
   the new one — no replay either way — and transactions after *H*, or pending, are signed again.

The old network may go on (a thief holding its keys can keep producing): it is a different network
id from here on. Nothing in the protocol can stop it; the announcement is what moves people.

**Not covered:** admin keys stolen. The thief can issue itself a generation, indistinguishable from
a legitimate one; the recovery set replaces the admin set, and the delay gives people time to
notice ([B004](DECISIONS.md#b004--recovery-set-narrow-slow-visible)).
