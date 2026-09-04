# Test strategy (purpose catalog, CI ladder)

**Tier:** ops

Purpose IDs (`L-*`) are the vocabulary for ledger qualification. Doctrine:
[TESTING.md](../architecture/TESTING.md).

Related: [AGENTS.md](../../AGENTS.md), [amp-transport.md](../amp-transport.md),
[development-budget-plan.md](../development-budget-plan.md) (Phase A: adversarial
ingress / invariants).

---

## Principles (ops)

1. **Purpose-first** — pick the question, then the cheapest layer.
2. **Cost order** — unit → in-process compose → deploy/multi-process smoke.
3. **Hard filter** — Amp memory fabric owns loss/reorder when it can.
4. **Promote failures downward** — see doctrine.
5. **Assert, don't observe** — smoke scenarios must fail the process on tip
   mismatch; soft observation-only passes are not enough.

---

## CI ladder (target)

| Gate | Contents | Status |
|------|----------|--------|
| **PR** | `ctest` (unit + Amp RPC compose) | Wired today via `scripts/ci-build.sh --with-tests` |
| **Nightly / manual** | L0/L1 multi-process smoke (`test-network.sh`, hardened `test-checkpoint-cycles.sh`) | Scripts exist; not CI-gated yet |
| **Weekly / manual** | Chaos restart, soak, capacity curves | Planned (`cost/flake` until green) |

Do **not** PR-block on probabilistic empty-slot waits. Use clock/leader inject in
unit/integration; keep multi-process smoke for process isolation and sync.

---

## Purpose inventory (v1)

| ID | Question | Home tier | Skip / notes |
|----|----------|-----------|--------------|
| **L-CONSENSUS-ELECT** | Same stakeholders + slot ⇒ same leader | Unit (`test_ouroboros_consensus`) | |
| **L-CONSENSUS-CLOCK** | Injected clock pins slot/epoch | Unit (`ClockOverridePinsCurrentSlot`) | |
| **L-CONSENSUS-FORCE** | Forced leader overrides election + validate | Unit (`ForceSlotLeaderOverridesElection`) + Integration (`ForcedLeader_ProducerAndPeerAcceptTip`) | Unlocks smokes without empty-slot lottery |
| **L-CONSENSUS-WRONG-LEADER** | Non-leader rejected on unsealed `addBlock` | Integration (`WrongLeader_UnsealedAddBlockRejected`) | |
| **L-CHAIN-SEQ** | Block sequence / hash / genesis rules | Unit (`test_chain`) | |
| **L-CHAIN-FEE** | Fee / spending power / renewals | Unit (`test_account_buffer`, chain) | |
| **L-NET-RPC** | Amp ledger RPC echo + Client framing | Integration (`test_amp_ledger_rpc`) | |
| **L-NET-LOSS** | RPC fails clean under total datagram loss | Integration (`RoundTripFailsWhenDatagramsDropped`, `SuccessThenLossFailsSecondRoundTrip`) | Docker netem = `covered-above` later |
| **L-NET-REORDER** | RPC survives reorder window | Integration | `glue-gap` — extend MemoryDatagramIo harness |
| **L-SMOKE-L0** | Binaries boot; client status reaches beacon | Smoke (`test-network.sh`) | Not CI-gated yet |
| **L-COMPOSE-TIP** | Forced-leader block accepted by peer tip | Integration (`ForcedLeader_ProducerAndPeerAcceptTip`) | In-process stand-in for L-SMOKE-L1 |
| **L-SMOKE-L1** | Beacon→relay→miner produces tip | Smoke | Assert tip progress (not only process up) |
| **L-SMOKE-LATEJOIN** | Late miner tip catches beacon tip | Smoke (`test-checkpoint-cycles.sh` scenario 3) | Hard `nextBlockId` equality assert |
| **L-SMOKE-CHAOS** | Restart relay/miner; recover or fail clean | Smoke | `cost/flake` — nightly later |
| **L-ADV-INGRESS** | Malformed / oversize / replay at RPC ingress | Unit + integration | Phase A priority; expand adversarial vectors |
| **L-FORK-CHOICE** | Competing slot blocks / reorg | — | `non-goal` until fork choice ships |
| **L-MULTI-BEACON** | Inter-beacon sync / BFT | — | `non-goal` (see SERVER.md future) |

---

## Multi-process smoke scripts

| Script | Role |
|--------|------|
| `test-network.sh` | L0/L1 local testnet bring-up (beacon + miners; optional HTTP) |
| `test-checkpoint-cycles.sh` | Checkpoint / late-joiner scenarios; **must assert tip equality** on late join |
| `deploy/` compose | Manual image smoke |

When adding a scenario: give it a purpose ID, a hard assert, and a skip reason if
it stays manual.

---

## Fixture conventions

- Unique temp dirs under `std::filesystem::temp_directory_path()`.
- Destroy stores / mesh runtimes before wiping dirs (parent-only teardown).
- Prefer `TEST_F` + `std::unique_ptr` over stack locals that outlive `remove_all`.
