# Ledger network topology — design guidance

**Status:** agreed long-term guidance (2026-09-02).  
**Scope:** beacon / gateway / miner interaction over the fleet ledger RPC, independent of
transport binding (AMP today; libp2p at the pp-browser edge per
[platform-integration.md](PLATFORM_INTEGRATION.md)).  
**Related:** [design.md](../product/DESIGN.md) (roles & consensus), [AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md)
(wire format & config field names), [NAME_DIRECTORY.md](../product/NAME_DIRECTORY.md) (DomainIndex /
NameIndex — future terminal-owned registries).

This document is **normative guidance** for making the chain protocol robust, efficient, and
reliable. Implementation may lag; new work should move toward these properties rather than
away from them.

---

## 1. Problem statement

The fleet has three operational roles — **terminal beacon**, **gateway**, and **miner** — but
participants on the wire must not need to know which role they reached. Gateways may chain;
miners may run at the edge; ops may insert or move hops without reconfiguring client logic.

Robustness therefore cannot depend on peer **role labels**. It depends on:

1. A single terminal authority for canonical state.
2. A uniform RPC surface at every hop.
3. Cryptographic chain validation at every node.
4. Explicit **realization** semantics for mutations.
5. Multi-upstream consistency checks where participants configure more than one endpoint.

---

## 2. Terminology

| Term | Meaning |
|------|---------|
| **Terminal beacon** | The sole node that commits canonical chain state and the authoritative stakeholder registry. Often on a private network; not required to be reachable from the public internet. |
| **Gateway** | Any node that exposes the fleet ledger RPC downstream and has an upstream path toward the terminal (`pp-relay`, edge relay, pp-node, …). Gateways cache and forward; they are not final authority. |
| **Participant** | Miner, client, or edge miner that **consumes** upstream endpoints. Uses the same RPC whether upstream is a gateway or terminal. |
| **Opaque upstream** | A configured multiaddr (e.g. miner `beacons[]`, gateway `beacon`) with **no role metadata**. Naming is historical; semantics are “upstream ledger endpoint.” |
| **Realization** | A mutation is **realized** when the terminal beacon has accepted it into authoritative state (or an equivalent synchronous commit on the critical path). |
| **Write-through** | Gateway returns success on a mutation only after its upstream path reports success for that mutation. |
| **Watermark** | Metadata describing how fresh a replica is: head height/hash, checkpoint id, registry version, estimated lag behind terminal. |

Config field names (`beacons[]`, `beacon`) remain for compatibility; this document uses
**upstream** when describing behavior.

---

## 3. Topology model

```text
┌─────────────────────────────────────────────────────────────┐
│  Participants (miners, clients, edge miners)                  │
│  — uniform client; opaque upstream[]; validate chain locally │
└───────────────────────────┬─────────────────────────────────┘
                            │  same RPC at every hop
┌───────────────────────────▼─────────────────────────────────┐
│  Gateways (relay, edge relay, pp-node, …)                   │
│  — cache + forward; may chain; never final authority        │
└───────────────────────────┬─────────────────────────────────┘
                            │  write-through for mutations
┌───────────────────────────▼─────────────────────────────────┐
│  Terminal beacon                                            │
│  — sole writer of canonical chain + authoritative registry  │
└─────────────────────────────────────────────────────────────┘
```

```mermaid
flowchart TB
  subgraph participants["Participants"]
    M1[Miner]
    M2[Edge miner]
    C[Client]
  end
  subgraph gateways["Gateways (may chain)"]
    R1[Regional relay]
    R2[Edge relay]
  end
  T[Terminal beacon]
  M1 & M2 & C --> R1 & R2
  R1 --> R2
  R2 --> T
```

**Deployment intent**

- Terminal beacon: scarce, private, high trust; validates everything.
- Gateways: public-facing; small curated set operated by the org; shield the terminal.
- Miners: customer-facing producers; may sit at the edge; same upstream abstraction as
  datacenter miners.

**Chaining:** a gateway’s upstream may be another gateway. No hop inspects whether the next
hop is terminal. Only the last hop in a successful write path performs realization.

**Connection direction (recommended):** gateways initiate or maintain associations toward the
terminal across private links; participants dial gateways on public multiaddrs. The terminal
should not need a public listen address.

---

## 4. Design principles

| # | Principle | Rationale |
|---|-----------|-----------|
| P1 | **Uniform RPC** | One handler surface (`/pp-ledger/rpc/1.0.0`); no role-specific protocol forks. |
| P2 | **Single writer** | One terminal; avoids split-brain commits. |
| P3 | **Local verification** | Every node validates blocks and transactions it applies; gateways get no trust discount. |
| P4 | **Opaque upstream** | Participants configure endpoints, not roles; ops owns topology. |
| P5 | **Write-through mutations** | Downstream success implies terminal realization (or gateway must error). |
| P6 | **Chain-anchored trust** | Participants pin network identity; they trust cryptography and consistency, not peer labels. |
| P7 | **Separation of planes** | Control, sync, write, query, and gossip have different consistency and caching rules. |
| P8 | **Fail safe on ambiguity** | Fork, wrong network, or divergent upstreams → stop mining / reject upstream, do not guess. |

---

## 5. Core invariants

These are non-negotiable for a correct deployment:

| ID | Invariant |
|----|-----------|
| **I1** | Only the terminal beacon commits canonical chain state. |
| **I2** | Gateways return success on a mutation only if the terminal accepted it on the forwarding path (write-through). |
| **I3** | Every node cryptographically validates every block and transaction before applying it. |
| **I4** | Every participant pins `network_id` and a trusted genesis or checkpoint anchor; foreign chains are rejected. |
| **I5** | Slot time used for mining is derived from terminal-calibrated clock, not raw local wall clock alone. |
| **I6** | The stakeholder / miner registry is terminal-owned; gateway copies are replicas with a defined freshness bound. |

Violations of I2 (e.g. local-only registration on a gateway) or I4 (blind trust of the first
upstream) undermine the whole model regardless of transport security.

---

## 6. Logical planes

One protocol id is sufficient on the wire; behavior is split by **plane**:

| Plane | Purpose | Examples | Consistency | Gateway may cache? |
|-------|---------|----------|-------------|------------------|
| **Control** | Join, time, health | `REGISTER`, `CALIBRATION`, `STATUS` | Strong (terminal-backed) | No for `REGISTER`; calibration only if locked to terminal |
| **Sync** | Catch up chain | `BLOCK_GET`, checkpoint fetch | Strong per block (verified) | Yes, with watermarks |
| **Write** | Submit work | `BLOCK_ADD`, stake updates | Strong (realized at terminal) | No — forward |
| **Query** | Read state | `ACCOUNT_GET`, `TX_*` | Eventual within lag bound | Yes |
| **Gossip** | Miner mesh hints | block announcements, mempool (future) | Eventual | N/A (peer-to-peer) |

Efficiency comes from caching **Sync** and **Query**, not from weakening **Control** or
**Write**.

---

## 7. Realization contract (normative)

For each RPC class, define **who may answer** and **what success means**:

| RPC / class | Answered by | Success means | Gateway rule |
|-------------|-------------|---------------|--------------|
| `REGISTER` | Terminal | Miner appears in authoritative registry | **Write-through**; return terminal `BeaconState` |
| `CALIBRATION` | Terminal (or gateway mirroring terminal clock) | Timestamp reflects terminal slot time | Forward, or serve only if synced within ε ms |
| `STATUS` | Gateway OK | Head, checkpoint, epoch consistent with terminal within lag | Cache with watermark |
| `MINER_LIST` | Terminal registry | Matches terminal at `registry_version` | Forward or replica; must not serve stale registry for leader routing |
| `BLOCK_GET` | Gateway or terminal | Block verifies under pinned network + parent link | Serve locally if present; else fetch upstream |
| `BLOCK_ADD` | Terminal | Block committed to canonical chain | **Write-through**; propagate terminal response |
| `BLOCK_WAIT` | Gateway or terminal | Reply carries the answerer's `nextBlockId` | Serve from own tip; a gateway's tip follows its own `BLOCK_WAIT` upstream (§10.2) |
| `TX_ADD` | Terminal pending pool | Tx held for the slot leader | **Write-through** up the tree; §10.3 |
| `TX_PULL` | Terminal pending pool | Pending txs for the leader's slot | Pass through to the terminal |
| `ACCOUNT_GET`, `TX_*` | Gateway OK | State at height ≤ terminal head − read_lag | Cache allowed |

**Mutations that must reach the terminal:** chain commits (`BLOCK_ADD`), registry changes
(`REGISTER`, stake updates). All other participant-facing writes either route to the slot
pool (`TX_ADD`) or are reads.

Gateways must **forward terminal errors verbatim** (e.g. `FORK_DETECTED`, `WRONG_NETWORK`,
`STALE_REGISTRY`) so participants can react without knowing hop count.

### 7.1 Request direction (normative)

Each node knows its upstreams by peer id (the `/p2p/<PeerId>` of its configured
multiaddrs) and AMP authenticates every request's sender, so a node can tell
whether a request came from its own upstream. `Server::isAllowedFrom` applies
one table for all roles before any handler runs:

| Request | From downstream | From own upstream |
|---------|-----------------|-------------------|
| `BLOCK_ADD`, `REGISTER`, `TX_ADD` (writes travelling up to the terminal) | Accepted | **Refused** |
| `BLOCK_WAIT` (blocks travel down) | Accepted | **Refused** |
| `MINER_LIST`, `TX_PULL` (the registry and pool live upstream) | Accepted | **Refused** |
| Everything else | Accepted | Accepted |

Miners do not accept `BLOCK_ADD` at all: they learn blocks by syncing from
their upstream, never by having blocks pushed into them.

The same place sets each request's priority (`Server::laneFor`): reads such as
`BLOCK_GET` and `BLOCK_WAIT` (downstream sync) and account / history queries go to a low-priority
lane with its own capacity and a per-peer cap, so serving downstream sync never
blocks the node's own work (docs/architecture/THREADING.md).

---

## 8. Network anchor (participant config)

Every participant should pin network identity **independently of upstream multiaddrs**:

```json
{
  "network_id": "pp-mainnet-1",
  "genesis_hash": "<hex>",
  "trusted_checkpoint": {
    "id": 42,
    "block_hash": "<hex>"
  }
}
```

**Bootstrap sequence**

1. Associate with an upstream (opaque multiaddr).
2. Call `STATUS` (or equivalent); verify `network_id` and checkpoint ≥ trusted minimum.
3. Reject upstream on mismatch — wrong network or likely eclipse.
4. Sync from `max(local_tip, trusted_checkpoint)` toward head.

This is how participants stay safe **without** learning whether upstream is a gateway or
terminal.

---

## 9. Chain sync

Sync is the primary reliability challenge under opaque gateways.

### 9.1 Modes

| Mode | When | Behavior |
|------|------|----------|
| **Cold start** | New node | Anchor verify → sync checkpoint → blocks to head |
| **Catch-up** | Behind by many blocks | Range fetch in batches (not one RPC per block) |
| **Tip follow** | Near head | Poll head watermark; fetch only new blocks |
| **Reorg repair** | Parent hash mismatch | Walk back to common ancestor; replay forward |
| **Gap fill** | Missing intermediate block | Fetch range `[from, to]` without full resync |

### 9.2 Watermarks

Replicas (gateways and participants) track:

| Field | Use |
|-------|-----|
| `head_height` / `head_hash` | Highest contiguous validated block |
| `checkpoint_id` | Latest stable checkpoint (see [design.md](../product/DESIGN.md)) |
| `registry_version` | Generation of terminal miner/stake registry |
| `replica_lag` | Estimated blocks (or ms) behind terminal |

**Policy knobs (ops-tunable):**

- `max_read_lag` — gateway may serve queries if lag ≤ this.
- `max_write_lag` — gateway should reject or defer write-through if lag exceeds this (optional).

### 9.3 Multi-upstream selection

Participants configure `upstream[]` (miner `beacons[]`). Recommended algorithm:

```text
1. STATUS from each upstream
2. Reject any failing network_id / checkpoint / genesis checks
3. Score survivors: height, RTT, error rate, hash agreement at common height
4. Sync from highest-scoring upstream
5. Spot-check random blocks against a second upstream
6. On hash divergence at same height → blacklist upstream, failover, do not mine
```

Broadcasting writes (`BLOCK_ADD`) should use **all** healthy upstreams (or an ordered list
with retry), not only the sync source.

### 9.4 Gateway chain sync

Gateway G₁ → G₂ → terminal: each gateway syncs from its **configured upstream**, not from
downstream miners. A gateway’s read freshness is bounded by its upstream path. For deep
chains, consider:

- Limiting chain depth in production, or
- A **gateway mesh** for block replication between regional gateways (read path only),
  reducing serial latency.

### 9.5 Checkpoints

Align with the checkpoint model in [design.md](../product/DESIGN.md): new participants may join from a
trusted checkpoint and replay only subsequent blocks. Sync logic should treat checkpoint
boundaries as first-class (fast join, pruning compatibility).

---

## 10. Miner coordination

SlotCommittee slot leadership requires more than “leader posts `BLOCK_ADD` upstream.”

### 10.1 Block propagation — up to commit, down by waiting

| Path | Purpose | Required |
|------|---------|----------|
| **A. Leader → terminal** (`BLOCK_ADD`, upward only) | Canonical commit | **Yes** — only realization path |
| **B. Upstream → downstream** (`BLOCK_WAIT`, downward only) | Fast propagation to gateways and miners | Yes (falls back to C) |
| **C. Participant → upstream sync** (periodic) | Safety net | **Yes** — catches anything B missed |

```mermaid
sequenceDiagram
  participant L as Slot leader
  participant G as Gateway
  participant T as Terminal
  participant V as Other miners

  V->>G: BLOCK_WAIT {knownNext}
  G->>T: BLOCK_WAIT {knownNext}
  L->>G: BLOCK_ADD
  G->>T: forward (write-through)
  T-->>G: committed
  G-->>L: success
  G-->>V: BLOCK_WAIT reply {nextBlockId}
  V->>G: BLOCK_GET (BlockSync)
```

### 10.2 BLOCK_WAIT (downward propagation)

- Each relay and miner keeps one `BLOCK_WAIT {knownNextBlockId}` open to its
  upstream (`UpstreamTipWatch`). The upstream (beacon or relay) replies with its
  `nextBlockId` as soon as that passes `knownNextBlockId`, or unchanged after a
  hold of T/2 (`NetworkTuning::blockWaitHold`); the downstream then re-arms.
- When the reply shows the upstream ahead, the downstream syncs (`BlockSync`,
  `BLOCK_GET` from that upstream) and applies blocks only after full validation (I3).
- **Downward only, once per block:** only an upstream answers a wait (a wait
  from one's own upstream is refused, §7.1), and the next wait starts from the
  `nextBlockId` already reported, so a block is announced to each downstream
  once, with no list of downstreams and no dialing toward them (NAT-friendly).
- Cheap to misuse: one wait per peer is held (a newer one answers the older);
  waits sit in the low-priority lane; a reply only triggers a sync from the
  node's own upstream, which BlockSync rate-limits.
- Miners do not serve `BLOCK_WAIT`. An upstream that does not serve it makes the
  watch back off (1 s → 30 s); path C keeps the node in sync.

### 10.3 Transactions — up to the terminal pool, pulled by the leader

| Step | Actor | Action |
|------|-------|--------|
| 1 | Client | `TX_ADD` to a relay or the terminal (pp-client `-b`, pp-http) |
| 2 | Relay / miner | Pass `TX_ADD` up to its upstream (write-through); never to another miner |
| 3 | Terminal | Hold it in the pending pool (`TxPool`: bounded, deduplicated, expires at the tx's validity window or after 10 min) |
| 4 | Slot leader | At its slot's start, `TX_PULL {slot}` through its upstream (relays pass it through); waits up to min(1 s, slot/3) before producing |
| 5 | Leader | Pool what applies; include in the block; `BLOCK_ADD` up to the terminal |
| 6 | Terminal | Drop the block's records from the pending pool |

No miner learns another miner's address: a transaction never goes miner →
miner. The terminal drops a transaction only once a committed block includes it
(or it expires), so one a leader missed or could not fit reaches the next
leader. `TX_ADD` and `TX_PULL` are refused from a node's own upstream (§7.1).

**Leader pool.** A leader validates every transaction it pools against its
buffer for the current slot, rebuilt from the tip whenever the tip moves. When
a slot rebuilds the pool, pending transactions that no longer apply (included
elsewhere, balance spent, validity window ended) are dropped rather than
aborting the slot. A rejection by the leader's pool is final, like a forward
rejection; out-of-order dependent transactions must be resubmitted.

### 10.4 Registration and stake

`REGISTER` must be **write-through** and **authenticated**:

- Registrant proves control of mining keys: the record is signed by the miner
  account's keys (same rule as its transactions: on-chain `publicKeys` /
  `minSignatures`). The terminal also refuses an `issuedAt` more than 5 min from its
  clock or not newer than the recorded one (replay). Gateways pass records on
  unchanged and cannot forge them. Signing format: WIRE_SCHEMA.md.
- Terminal records `{ miner_id, last renewal }` (no network address) and a `registry_version`.
- `MINER_LIST` includes `registry_version` so participants detect stale gateway cache.
- **Renewal:** miners re-register every 60 s; the terminal stamps each record
  and drops one not renewed within 5 min (`MinerRegistry`). `registry_version`
  changes only when the list does (join, expiry), not on renewal.
- **Address exposure:** miners have no published address. Registrations
  carry none, `MINER_LIST` returns ids only and is accepted only from
  downstream (§7.1), and transactions reach the leader by `TX_PULL` (§10.3).
  Miners dial out to their relays (sentry pattern) and need no public listen
  address, so the public leader schedule cannot be used to flood the next slot
  leader.

---

## 11. Time and slots

Slot leadership is time-sensitive.

1. `CALIBRATION` returns terminal time plus `next_block_id` / slot / epoch.
2. Miner estimates `offset_ms` using multiple RTT samples; prefer low-RTT samples.
3. Re-calibrate on epoch boundaries and when observed blocks disagree with expected slot.
4. If multiple upstreams disagree on time beyond a threshold → **do not mine** until resolved.

Gateways must not invent time. They forward terminal calibration or serve from a clock
explicitly synchronized to the terminal within ε.

---

## 12. Security model (topology-opaque)

### 12.1 What cryptography provides

- Invalid blocks and transactions are rejected locally.
- Forging canonical history requires breaking signatures and consensus rules.

### 12.2 What participants must add

| Threat | Mitigation |
|--------|------------|
| Wrong network / eclipse | Network anchor (§8); multi-upstream hash comparison |
| Stale reads | Watermarks; `max_read_lag`; cross-check `STATUS` |
| Withheld writes | Multi-upstream `BLOCK_ADD`; `BLOCK_WAIT` + sync detects missing blocks |
| Fake registration | Signed `REGISTER`; terminal verification |
| Gateway censorship | Multiple upstreams; monitoring head lag |
| Flooding / abuse | Per-PeerId rate limits on gateways (ops layer) |
| Targeted flooding of the next slot leader | Miner addresses not published (§10.4); relay-only access |

Participants do **not** authenticate “this peer is the real beacon.” They authenticate **chain
continuity and network identity**.

### 12.3 Ops-only controls (invisible on the wire)

These do not violate uniform RPC or opaque upstream:

- Terminal: allowlist gateway PeerIds for inbound associations.
- Gateway: rate limits, connection caps, payload size limits.
- Private terminal: no public multiaddr; gateways dial inward.

---

## 13. Reliability patterns

### 13.1 Upstream health

Track per upstream: height rank, error rate, RTT, hash agreement with peers. Use for sync
source selection, calibration, and ordered write retry.

### 13.2 Idempotency

| Operation | Idempotent? | Note |
|-----------|-------------|------|
| `BLOCK_ADD` (same hash) | Yes | Terminal and gateways answer a block they already hold (same id and hash) with success, without validating it again; a gateway does not forward it again. A different block at that id is refused (`BlockAddPolicy.h`) |
| `REGISTER` (same miner_id) | Yes | Upsert semantics |
| `TX_ADD` | Yes (pool) | The terminal pool holds one copy per record; the chain refuses a repeated idempotent id |

Gateways should safely forward retries.

### 13.3 Persistent upstream associations

Gateways should maintain long-lived transport associations to upstream(s) with reconnect and
backoff. This improves write-through latency and makes “realized at terminal” easier to
reason about than per-request dial.

### 13.4 Failure modes

| Failure | Expected behavior |
|---------|-------------------|
| Primary upstream down | Failover to next in `upstream[]` |
| Gateway down | Participant uses alternate upstream |
| Terminal unavailable | Writes fail; reads from gateway within `max_read_lag` if policy allows |
| Partition / fork | Hash mismatch → reject upstream; **do not mine** |
| Missed slot | Empty slot; chain continues per SlotCommittee |

---

## 14. Efficiency guidelines

| Area | Guidance |
|------|----------|
| Block sync | Batch / range API (`BLOCK_GET_RANGE` or equivalent); avoid one block per RPC at scale |
| Checkpoints | Sync to checkpoint first, then tail blocks |
| Gateway cache | Block cache + account reads at or behind head |
| Gossip | Announce hash only; fetch on demand |
| Miner storage | Partial history acceptable when checkpoint-anchored |
| Relay chains | Prefer shallow chains or gateway mesh for read replication |

Optimize only after invariants I1–I6 hold.

---

## 15. Recommended protocol extensions

Small, role-neutral extensions that support the model:

| Extension | Purpose |
|-----------|---------|
| `STATUS` v2 fields | `network_id`, `head_hash`, `registry_version`, `replica_lag` |
| `BLOCK_GET_RANGE` | Efficient catch-up |
| Signed `REGISTER` | Registry authentication |
| Stable error codes | `WRONG_NETWORK`, `FORK_DETECTED`, `STALE_REGISTRY`, `UPSTREAM_LAG` |

None of these expose peer role to participants.

---

## 16. Implementation phasing (guidance)

| Phase | Focus | Unlocks |
|-------|-------|---------|
| **P0** | I2 write-through for all mutations; network anchor; multi-upstream `STATUS` compare | Correctness |
| **P1** | Batch sync, watermarks, reorg handling | Reliable catch-up |
| **P2** | Downward block propagation (`BLOCK_WAIT`, done); signed `REGISTER` | Multi-miner liveness |
| **P3** | Gateway cache policy, persistent upstream, range reads | Efficiency |
| **P4** | Gateway mesh, ops ACL on transport | Production hardening |

Order matters: P0 before optimizing read paths.

---

## 17. Non-goals

- Exposing relay vs terminal role on the wire or in participant config semantics.
- DHT or open peer discovery for fleet infrastructure nodes (curated multiaddrs instead).
- Embedding libp2p inside pp-ledger core ([platform-integration.md](PLATFORM_INTEGRATION.md)).
- Gateways committing canonical state without terminal realization.
- Trusting upstream IP or DNS instead of chain anchors and hashes.

---

## 18. Summary

**Terminal beacon** is the sole commit point for chain and registry. **Gateways** are
verify-and-cache forwarders over a **uniform RPC**. **Participants** stay safe by pinning
network identity, requiring write-through mutations, validating all data locally, using
multiple opaque upstreams for consistency, and waiting on their upstream (`BLOCK_WAIT`) so
new blocks reach them at once.

Ops owns topology; the protocol owns correctness.
