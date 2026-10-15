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
| Production window (B) | Seal after the slot, deliver within `min(slot/3, 2 s)`; beacon refuses late blocks — see [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md#slot-timing) |
| Miner registration (A1) | Registered miners only; due miners produce; missed due slots deregister — see [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md#miner-registration-and-liveness) |
| Empty heartbeats (C) | Genesis `heartbeatSlots`; idle leaders seal empty blocks when tip lag ≥ threshold — see [WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md#empty-heartbeat-blocks) |
| Out-of-process test injectors (F) | **Not needed** — multi-process L1 / LATEJOIN are deterministic without forcing leaders (see F) |

---

## Open items

### A. Miner-registered committee — **done**

**Shipped (A1):** only registered miners are elected. Accounts register with `T_MINER_REGISTER`
(fee + `minerMinStake`) and revoke with `T_MINER_REVOKE`; either applies at the next epoch's
snapshot. Liveness: a miner is due after `minerDueAfterSlots` without a block and must produce
(empty if need be) in a slot it leads; `minerMaxMissedSlots` due misses in a row deregister it,
with a re-registration cool-down. No bonds or slashing (A2 stays future work). Normative:
[WIRE_SCHEMA.md — Miner registration and liveness](../contracts/WIRE_SCHEMA.md#miner-registration-and-liveness).

---

### B. Slot production window — **done**

**Shipped:** the leader collects for its whole slot, seals once it has ended, and must deliver
within `min(slot / 3, 2 s)`; the beacon refuses a block past that deadline (the slot is empty).
The window is shorter than a slot, so the next leader always seals on a settled tip. Normative:
[WIRE_SCHEMA.md — Slot timing](../contracts/WIRE_SCHEMA.md#slot-timing).

---

### C. Empty / heartbeat blocks — **done**

**Shipped:** Genesis `BlockChainConfig.heartbeatSlots` (`GenesisAccountMeta` v3). Idle
slot leaders (no renewals / mempool) seal a normal empty block when
`currentSlot - tip.slot >= heartbeatSlots`. `0` disables. Beacon `--init` default
when omitted: `heartbeatSlots = slotsPerEpoch`. Validators reject premature
empties (and all empties when disabled).

**Settled interpretation:** any non-empty body (txs or renewals) counts as
liveness and bypasses the empty-lag rule; heartbeat is not an empty-only duty
cycle. See [BLOCK_PIPELINE.md](BLOCK_PIPELINE.md#empty-heartbeat-settled-any-work-counts).

Normative: [WIRE_SCHEMA.md — Empty heartbeat blocks](../contracts/WIRE_SCHEMA.md#empty-heartbeat-blocks).
Miner gate: `Miner::produceBlock`; validation: `checkBlockBodyPolicy` /
`validateEmptyHeartbeatPolicy`. Pipeline: [BLOCK_PIPELINE.md](BLOCK_PIPELINE.md).


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
5. **Schedule** — promotion does **not** change `SlotCommittee` math; it changes **who realizes** blocks — including who enforces the delivery deadline ([Slot timing](../contracts/WIRE_SCHEMA.md#slot-timing))  

**Recommendation:** Spec **1–5** as an ops/topology ADR before implementation. Complexity is mostly role flip + fencing + config propagation, not election.

---

### F. Out-of-process test injectors (smoke) — **closed, not needed**

In-process: `SlotCommittee::forceSlotLeader` / `setClockOverride` (tests only) — unchanged.

**Outcome (2026-10-15):** with A1, B and C shipped, multi-process L1 / LATEJOIN no longer depend
on the slot lottery: 10 of 10 clean `--suite smoke` runs passed every stage. The failures that
had been read as lottery flakes were three other bugs, fixed instead of masked:

- Stale smoke data — a fresh genesis kept the relay's / miners' old ledgers (nodes on different
  chains); now wiped with the beacon.
- Miner halt — a re-pulled transaction after an abandoned block put two copies of one idempotency
  key in the draft and every later seal failed; the draft now refuses the duplicate.
- Relay link exhaustion — each exiting `pp-client` left its Amp link on the relay until the alive
  timeout, filling `max_links` (48) so new dials timed out; pp-cpp-amp v2.17.0 closes links on stop.

Reopen only if a scenario needs a *specific* leader schedule (e.g. scripting a miner that misses
its slots). Then prefer a deterministic rotation over the registered pool (keeps A1 liveness
rules), set identically on every node behind a test-only build option — not a per-node RPC.

---

## Suggested pickup order

1. **E** beacon failover mechanism doc → then code (the promoted node also takes over the delivery deadline)  

---

## Related docs

- [wire-schema.md — Leader election](../contracts/WIRE_SCHEMA.md#leader-election-live)  
- [DESIGN.md](../product/DESIGN.md) — beacon / relay / miner roles  
- [LEDGER_TOPOLOGY.md](LEDGER_TOPOLOGY.md)  
- [ops/TEST_STRATEGY.md](../ops/TEST_STRATEGY.md) — L-SMOKE-*  
- [src/consensus/README.md](../../src/consensus/README.md)
