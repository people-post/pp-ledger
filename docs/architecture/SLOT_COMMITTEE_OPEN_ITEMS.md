# SlotCommittee — open items

**Status:** open (design decisions deferred)  
**Live path:** [`SlotCommittee`](../../src/consensus/SlotCommittee.h) — equal-weight top‑N committee lottery under beacon authority  
**Normative election rules:** [WIRE_SCHEMA.md — Leader election](../contracts/WIRE_SCHEMA.md#leader-election-live)  
**Context:** Rename from “Ouroboros” and equal-weight top‑N are **done**. Items below are remaining product/protocol choices.

Ouroboros remains a **literature reference** only (not a product goal).

---

## Done (do not reopen without cause)

| Item | Outcome |
|------|---------|
| Naming | Live type/docs use **`SlotCommittee`**; hash domain `pp-ledger/slot-committee/v2` |
| Equal-weight top‑N | **Designed behavior** — stake gates committee *entry*, not weight inside the pool |
| Authority model (intent) | **One active beacon at a time**; relays may be promoted if beacon is lost — *mechanism still TBD* (see E) |
| Epoch seed (D) | Header `epochSeed` (Block v5); derivation in `EpochSeed.*`; election mixes seed — see [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md#epoch-seed) |

---

## Open items

### A. Miner-registered committee

**Lean:** Prefer committee = top‑N **among miner-registered accounts**, not every rich account.

**Why:** Open top‑N elects accounts with no running miner → quiet slots / flaky L1–LATEJOIN even when Amp dial works.

**Options (complexity ↑):**

| ID | Approach | Complexity | Notes |
|----|----------|------------|-------|
| A0 | Ops-only: only fund registered miner ids in genesis/smoke | Low | Lab help only; no open-network guarantee |
| A1 | Account flag / register-miner tx; filter in `getEligibleLeaderPool()` | Medium | **Recommended first cut** |
| A2 | Bonded / slashable miner set | High | Economics + failure modes; later |

**A1 sketch (when picked up):**

1. Persist “may mine” on account or small registry included in epoch stake snapshot  
2. Filter: positive stake **and** registered  
3. Validate `slotLeader` ∈ registered set for that epoch  
4. Register / revoke rules + epoch boundary semantics  

**Recommendation:** Prototype **A1** before bonds (A2). Soft A0 can stay for smoke fixtures in parallel.

---

### B. Slot production window

**Today:** `isSlotBlockProductionTime` ≈ last **1 second** of the slot.

**Lean (discussion):** Not load-bearing history; revisit for ops/smoke ergonomics.

| | Keep last ~1s | Widen (e.g. 2nd half or most of slot) |
|--|---------------|----------------------------------------|
| Pros | Less early duplicate work; simple “seal near end” | Fewer missed slots under load/skew; better with short smoke slots |
| Cons | Easy to miss; harsh when `slotDuration` is small | Earlier produce; need strict beacon reject of bad leaders/tips |
| Safety | Not cryptographic by itself | Still = validate leader + timestamp ∈ `[start, end)` |

**Recommendation:** Prefer **widen** (produce after a small post-start grace through slot end). With a single beacon, “first valid wins” + leader checks are enough; last-1s is optional caution, not consensus math.

---

### C. Empty / heartbeat blocks

**Today:** Leader with no pending txs and no renewals does **not** seal → tip can stall while leadership is healthy.

| Allow empty seals (or rare heartbeats) | Keep “block only with work” |
|----------------------------------------|-----------------------------|
| Clear liveness; simpler tip smokes | Leaner history; tip ≈ economic activity |
| Need policy for empty-block fees/storage | Smoke/tip monitors stay mempool-coupled |

**Recommendation:** Decide explicitly. For beacon-centered ops, a **rare heartbeat** or “empty seal allowed when leader” is the usual fix; alternatively keep lean history and treat tip progress as `covered-below` + force-leader for multi-process tests.

---

### D. Leader predictability / epoch nonce — **done (public seed)**

**Shipped:** Block header `epochSeed` (32 bytes, `CURRENT_VERSION` **5**). Epoch‑0 seed from genesis config digest; later epochs from prior seed + lookback tip material (K=8) + stake snapshot. Lottery domain `pp-ledger/slot-committee/v2` mixes `slot`, `epoch`, and `epochSeed`.

Leaders for an epoch become knowable once that epoch’s seed is public (still not private VRF). Further hardening (private leader proofs) remains optional later.

Normative rules: [WIRE_SCHEMA.md — Epoch seed](../contracts/WIRE_SCHEMA.md#epoch-seed) / [Leader election](../contracts/WIRE_SCHEMA.md#leader-election-live).

---

### E. Beacon loss → relay promotion

**Confirmed intent:** One beacon at a time; a relay that holds the chain **may** become beacon if the active beacon is lost.

**Still undefined (write a short design note before coding):**

1. **Detection** — how the fleet decides beacon is lost (ops, relay quorum, sync timeout, …)  
2. **Choice** — which relay is promoted (pre-designated backup, manual, highest checkpoint, …)  
3. **Cutover** — freeze writes, role flip, republish Amp listen multiaddr / peer id, update miner `beacons[]` / relay upstream  
4. **Fencing** — old beacon must not return as a second tip (generation id / epoch fence / …)  
5. **Schedule** — promotion does **not** change `SlotCommittee` math; it changes **who realizes** blocks  

**Recommendation:** Spec **1–5** as an ops/topology ADR before implementation. Complexity is mostly role flip + fencing + config propagation, not election.

---

### F. Out-of-process test injectors (smoke)

In-process: `SlotCommittee::forceSlotLeader` / `setClockOverride` (tests only).

Multi-process L1 / LATEJOIN stay `cost/flake` until one of: **A1** (bounded committee), **B/C** (window / empty seals), or a **test-only RPC/flag** to force leaders on running binaries.

**Recommendation:** Prefer product fixes (A1 + B and/or C) for real liveness; add out-of-process force-leader only if smoke must be deterministic before those land.

---

## Suggested pickup order

1. **A1** miner registration filter (highest ops/smoke leverage)  
2. **B** widen production window (cheap, local change)  
3. **C** empty/heartbeat policy (product call)  
4. **E** beacon failover mechanism doc → then code  
5. **F** as needed (D shipped)  

---

## Related docs

- [wire-schema.md — Leader election](../contracts/WIRE_SCHEMA.md#leader-election-live)  
- [DESIGN.md](../product/DESIGN.md) — beacon / relay / miner roles  
- [LEDGER_TOPOLOGY.md](LEDGER_TOPOLOGY.md)  
- [ops/TEST_STRATEGY.md](../ops/TEST_STRATEGY.md) — L-SMOKE-*  
- [src/consensus/README.md](../../src/consensus/README.md)
