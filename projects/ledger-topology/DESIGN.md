# Ledger topology — planned improvements

Agreed direction for the fleet topology that is **not built yet**. What exists today:
[docs/architecture/LEDGER_TOPOLOGY.md](../../docs/architecture/LEDGER_TOPOLOGY.md). Beacon
authentication and promotion are a separate project:
[beacon-authority](../beacon-authority/DESIGN.md).

New work should move toward these properties, not away from them.

---

## Sync

| Mode | When | Behavior |
|------|------|----------|
| **Reorg repair** | Parent hash mismatch | Walk back to the common ancestor; replay forward. Today nothing calls the stores' `rewindTo` and a node on a losing branch is stuck. (A bounded rewind to a beacon cutover height is planned in beacon-authority part 2.) |
| **Gap fill** | Missing intermediate block | Fetch `[from, to]` without a full resync |
| **Range catch-up** | Far behind | `BLOCK_GET_RANGE` (or equivalent) instead of one RPC per block |

### Watermarks

Replicas track `head_height` / `head_hash`, `checkpoint_id`, `registry_version` (all on `STATUS`
today) and **`replica_lag`** (not yet). Ops knobs:

- `max_read_lag` — a gateway serves queries only while its lag is at most this.
- `max_write_lag` — a gateway rejects or defers write-through beyond this (optional).

### Multi-upstream consistency (participants)

Today a miner picks the upstream with the highest `nextBlockId` and never compares hashes.
Target:

```text
1. STATUS from each upstream
2. Reject any failing network_id / checkpoint / genesis checks
3. Score survivors: height, RTT, error rate, hash agreement at a common height
4. Sync from the best
5. Spot-check random blocks against a second upstream
6. On hash divergence at the same height → blacklist the upstream, fail over, do not mine
```

Track per upstream: height rank, error rate, RTT, hash agreement — use them for sync source,
calibration and write retry order.

### Gateway chains

Each gateway syncs from its configured upstream, so read freshness degrades with chain depth.
Options: limit depth in production, or a **gateway mesh** replicating blocks between regional
gateways (read path only). Relays also take a single `beacon` today — a list with failover goes
with beacon-authority part 2.

---

## Time

1. Calibration already uses multiple RTT samples.
2. **Re-calibrate** on epoch boundaries and when observed blocks disagree with the expected slot
   (today: once at startup).
3. If upstreams disagree on time beyond a threshold → **do not mine** until resolved.

Gateways must not invent time: forward the terminal's, or serve from a clock synchronized to it
within ε.

---

## Protocol extensions

| Extension | Purpose |
|-----------|---------|
| `STATUS.replica_lag` | Freshness bound for reads |
| `BLOCK_GET_RANGE` | Efficient catch-up |
| Stable error codes | `WRONG_NETWORK`, `FORK_DETECTED`, `STALE_REGISTRY`, `UPSTREAM_LAG` — gateways forward them verbatim so participants react without knowing hop count |

None expose peer role to participants.

## Efficiency

| Area | Guidance |
|------|----------|
| Gateway cache | Block cache + account reads at or behind head |
| Gossip | Announce hashes only; fetch on demand (miner mesh / mempool hints — not built) |
| Relay chains | Prefer shallow chains or a gateway mesh |

Optimize only after the invariants in LEDGER_TOPOLOGY hold.

---

## Order

| Phase | Focus | Unlocks |
|-------|-------|---------|
| **P0** | Multi-upstream `STATUS` / hash comparison; stop mining on divergence | Correctness |
| **P1** | Range sync, watermarks, reorg handling | Reliable catch-up |
| **P2** | Gateway cache policy, persistent upstream, range reads | Efficiency |
| **P3** | Gateway mesh, ops ACL on transport | Production hardening |

P0 before optimizing read paths.
