# Testing doctrine (tiers, purposes, push-down)

**Tier:** architecture

How we design for testability and choose where coverage lives. **Ops inventory**
(purpose IDs, CI ladder, scripts) lives in [TEST_STRATEGY.md](../ops/TEST_STRATEGY.md).

This follows the same risk-accounting approach as pp-browser
(`docs/architecture/TESTING.md` there): cover **risk-weighted behaviors** at the
**cheapest sufficient tier**, not line-coverage at every tier.

---

## Goal

Prove consensus safety, ledger invariants, and fleet wiring under known failure
modes — with multi-process smoke reserved for packaging / process isolation /
topology that cheaper tiers cannot see.

---

## Decision rules

1. **Cheapest layer that answers the question** — name the purpose (`L-*`), then
   pick unit → in-process compose → multi-process smoke.
2. **Push complexity down** — put election, validation, codecs, and state-transition
   rules behind seams that gtests can own (e.g. `Ouroboros` clock/leader inject).
3. **Higher tiers verify wiring and environment** — they do not re-prove leader
   election math, fee rules, or hash commitments already locked below.
4. **Promote failures downward** — if a smoke script finds a policy bug, add a
   deterministic gtest in the same change set when possible; keep the smoke as
   packaging/topology evidence.
5. **Hard filter** — if in-process Amp memory fabric + virtual clock can reproduce
   it, it does **not** belong in multi-node smoke as the sole owner.

---

## Tiers

| Tier | Answers | Cost / flake |
|------|---------|--------------|
| **Unit** | Rules, codecs, stores, leader election, block validation | Lowest |
| **Integration** | In-process multi-role compose (Amp memory mesh, forced leaders) | Medium |
| **Smoke** | Packaged binaries, multi-process beacon/relay/miner, deploy caps | Highest |

**Kinds of risk** (orthogonal to tiers):

| Kind | Question |
|------|----------|
| **Correctness** | Right tip / reject invalid / honor stake rules |
| **Reliability / soak** | No hang, leak, or corrupt tip across restarts |
| **Capacity / SLO** | How much load before quality collapses |

---

## Skip taxonomy

Every high-risk behavior needs a home tier **or** an explicit skip:

| Skip | Meaning |
|------|---------|
| `covered-below` | Cheaper gtest owns the policy |
| `covered-above` | Only multi-process/env can prove it; smoke owns env part |
| `glue-gap` | Missing seam; prefer extracting a testable seam soon |
| `cost/flake` | Deferred until harness is deterministic / green |
| `non-goal` | Out of product scope for now |

---

## Injectors and in-process fabric

- **`Ouroboros::setClockOverride` / `forceSlotLeader` / `clearForcedSlotLeaders`** —
  deterministic slots and leaders so tests are not hostage to wall clock or
  empty-slot lottery.
- **Amp `MemoryDatagramIo` (`SetDropRate` / `DropNext`) + `VirtualClock`** — used by
  `src/network/amp/test/test_amp_ledger_rpc.cpp`; extend with reorder before Docker
  netem.
- Prefer thin `pp-client` probes over ad-hoc `curl` for smoke asserts.

---

## When a higher tier finds a bug

1. Can in-process loopback / a domain unit test reproduce it?
2. If yes — fix + lock below; keep smoke for env.
3. If no — document `covered-above` / `cost/flake` / true multi-netns-only in the
   purpose inventory.
