# Beacon authority — design

**Status:** proposed (part 1 decided — B001–B009; part 2 outlined)
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

## Where we are today

| Today | Consequence |
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

### Wire and storage impact (to promote into `docs/contracts/` when it ships)

- **Breaking, no migration** ([B008](DECISIONS.md#b008--breaking-genesis-change-no-migration)):
  nothing is released, so networks start from a new genesis.
- Genesis: recovery set (keys, threshold, delay in epochs) and the generation-1 certificate.
- New record types: beacon certificate; recovery action (announce / effect).
- `STATUS`, `BLOCK_WAIT`, `BLOCK_ADD` reply: carry the signed tip (and the current certificate on
  `STATUS`).
- Beacon disk: `keys/beacon-signing.key` / `.pub` (ba1), last signed height, latest signed tip.
- Nodes: highest certificate seen, latest verified signed tip, equivocation evidence.

### Settled questions

| Question | Answer |
|----------|--------|
| Beacon signing key vs Amp identity | Separate key ([B006](DECISIONS.md#b006--a-separate-beacon-signing-key)) |
| Recovery delay | ~30 days, in epochs; admin set frozen meanwhile ([B007](DECISIONS.md#b007--recovery-delay-about-a-month-admin-set-frozen-meanwhile)) |
| Existing networks | None released: breaking change, new genesis ([B008](DECISIONS.md#b008--breaking-genesis-change-no-migration)) |
| Signing cadence | Every block; keep only the latest signed tip ([B009](DECISIONS.md#b009--sign-every-block-keep-only-the-latest-signed-tip)) |

---

## Part 2 — promotion (outline)

All three cases issue or re-use a certificate under part 1's rules; nodes follow the highest
generation.

| Case | Keys | What happens |
|------|------|--------------|
| **1. Everything lost** — beacon key and admin keys | none | No protocol path can recover (one would serve an attacker too). **Re-genesis:** new keys and a new genesis that commits to the last good state (block hash + state root at a chosen height), published and announced out of band. The recovery set makes this rarer: losing the admin keys alone is case 2-like. |
| **2. Beacon stolen** — beacon key taken, admin keys safe | admin set | The admin set issues generation N+1 to a caught-up node, with `cutoverHeight` = the last tip trusted before the theft. Nodes on the thief's branch rewind to it; transactions after it are resubmitted (idempotency keys make that safe). Cut-off nodes get the certificate by file or config. |
| **3. Move the server** — all keys safe | all | **Plain operations** ([B003](DECISIONS.md#b003--moving-the-server-is-plain-operations)): stop writes, copy data and beacon key, start the new host, retire the old. Same generation. Rotating to a new key (generation N+1 with a clean cutover) stays the operator's choice. |

**Part 2 work** (designed after part 1): bounded rewind to a `cutoverHeight` (the only rewind —
not general fork choice); relays and miners accepting a list of beacon candidates; carrying the
calibrated network clock into the new beacon; how the transaction pool and miner registry rebuild
on the new beacon.

**Not covered:** admin keys stolen. The thief can issue itself a generation, indistinguishable from
a legitimate one; the recovery set replaces the admin set, and the delay gives people time to
notice ([B004](DECISIONS.md#b004--recovery-set-narrow-slow-visible)).
