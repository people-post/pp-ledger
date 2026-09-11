# Test strategy (purpose catalog, CI ladder)

**Tier:** ops

Purpose IDs (`L-*`) are the vocabulary for ledger qualification. Doctrine:
[TESTING.md](../architecture/TESTING.md).

Related: [AGENTS.md](../../AGENTS.md), [AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md),
[development-budget-plan.md](DEVELOPMENT_BUDGET.md) (Phase A: adversarial
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

## Local driver

Preferred entry point (mirrors pp-browser `pp_local_test.sh`):

```bash
./scripts/test/pp_ledger_local_test.sh run --suite unit
./scripts/test/pp_ledger_local_test.sh run --suite l0
./scripts/test/pp_ledger_local_test.sh run --suite l1
./scripts/test/pp_ledger_local_test.sh run --suite latejoin
./scripts/test/pp_ledger_local_test.sh run --suite smoke --down   # == l0 until Amp dial is green
./scripts/test/pp_ledger_local_test.sh run --suite image          # compose.smoke scaffold
./scripts/test/pp_ledger_local_test.sh stop   # or clear | status
```

Lifecycle only: `./scripts/test/pp_ledger_network.sh up|stop|clear|status`.

Root `test-network.sh` / `test-checkpoint-cycles.sh` are thin compatibility wrappers.

Smoke profile uses UDP ports **8617+** (beacon 8617, miners 8618+, relay 8622) and
dials via **ADP multiaddrs** (`pp-client --host '/ip4/.../adp/.../p2p/...'`).
Listen multiaddrs are scraped from `AMP ledger listener:` lines (`0.0.0.0` rewritten to
`127.0.0.1` for dialing). Configs are rendered from
`scripts/test/fixtures/*.tmpl`. Lab Amp identities live under
`$BUILD_DIR/test-smoke-lab-keys` (survive `stop`/`clear`; wipe with
`PP_LEDGER_SMOKE_WIPE_LAB_KEYS=1` on clear).

**L0 layers (fail-fast):** L0a PID tree → L0b beacon RPC via relay → L0c miner RPC.
`nextBlockId` is parsed as JSON (`python3`). Failures dump logs/configs under
`$TEST_DIR/artifacts/<timestamp>/`. `ensure_network` reuses only a healthy PID tree
(never a dead one).

**`--suite smoke`:** runs **l0** (process + RPC readiness). Use `--suite l1` /
`latejoin` explicitly for tip / late-join checks (slot-lottery flaky).

Multi-process OsUdp dial requires `LedgerAmpRuntime` to call `SetAcceptEnabled(true)`
(same as pp-browser `MeshHost`). In-process Amp memory fabric gtests still own RPC
correctness under loss/reorder (`L-NET-*`). L1/LATEJOIN remain sensitive to empty-slot
lottery until out-of-process forced-leader exists.

---

## CI ladder (target)

| Gate | Contents | Status |
|------|----------|--------|
| **PR** | `ctest` (unit + Amp RPC compose) | Wired today via `scripts/ci-build.sh --with-tests` |
| **Nightly / manual** | `--suite l0` / `l1` / `latejoin` via `pp_ledger_local_test.sh` | L0 green locally; L1/LATEJOIN `cost/flake` (slot lottery); **not CI-gated** |
| **Weekly / manual** | Chaos restart, soak, capacity curves | Planned (`cost/flake` until green) |
| **Image (manual)** | `deploy/compose.smoke.yml` + `--suite image` | Scaffold only |

Do **not** PR-block on probabilistic empty-slot waits. Use clock/leader inject in
unit/integration; keep multi-process smoke for process isolation and sync.

---

## Purpose inventory (v1)

| ID | Question | Home tier | Skip / notes |
|----|----------|-----------|--------------|
| **L-CONSENSUS-ELECT** | Same stakeholders + slot ⇒ same leader | Unit (`test_slot_committee`) | |
| **L-CONSENSUS-CLOCK** | Injected clock pins slot/epoch | Unit (`ClockOverridePinsCurrentSlot`) | |
| **L-CONSENSUS-FORCE** | Forced leader overrides election + validate | Unit (`ForceSlotLeaderOverridesElection`) + Integration (`ForcedLeader_ProducerAndPeerAcceptTip`) | Unlocks smokes without empty-slot lottery |
| **L-CONSENSUS-WRONG-LEADER** | Non-leader rejected on unsealed `addBlock` | Integration (`WrongLeader_UnsealedAddBlockRejected`) | |
| **L-CHAIN-SEQ** | Block sequence / hash / genesis rules | Unit (`test_chain`) | |
| **L-CHAIN-FEE** | Fee / spending power / renewals | Unit (`test_account_buffer`, chain) | |
| **L-NET-RPC** | Amp ledger RPC echo + Client framing | Integration (`test_amp_ledger_rpc`) | |
| **L-NET-LOSS** | RPC fails clean under total datagram loss | Integration (`RoundTripFailsWhenDatagramsDropped`, `SuccessThenLossFailsSecondRoundTrip`) | Docker netem = `covered-above` later |
| **L-NET-REORDER** | RPC survives reorder window | Integration (`RoundTripSurvivesReorderWindow`) | Amp `MemoryDatagramIo::SetReorderWindow` (random release) |
| **L-SMOKE-L0** | Binaries boot; client status reaches beacon | Smoke (`scripts/test/pp_ledger_l0_smoke.sh`) | L0a/b/c fail-fast; green on localhost OsUdp |
| **L-COMPOSE-TIP** | Forced-leader block accepted by peer tip | Integration (`ForcedLeader_ProducerAndPeerAcceptTip`) | In-process stand-in for L-SMOKE-L1 |
| **L-SMOKE-L1** | Beacon→relay→miner produces tip | Smoke (`scripts/test/pp_ledger_l1_smoke.sh`) | Short slots + tx inject; `cost/flake` (empty-slot lottery) |
| **L-SMOKE-LATEJOIN** | Late miner tip catches beacon tip | Smoke (`scripts/test/pp_ledger_latejoin_smoke.sh`) | Hard `nextBlockId` assert; `cost/flake` (slot lottery) |
| **L-SMOKE-IMAGE** | Packaged image compose boots / client probe | Smoke (`deploy/compose.smoke.yml`) | Scaffold; `--suite image` |
| **L-SMOKE-CHAOS** | Restart relay/miner; recover or fail clean | Smoke | `cost/flake` — nightly later |
| **L-ADV-INGRESS** | Malformed / oversize / replay at RPC ingress | Integration (`ClientRejectsOversizeRequest`, `TruncatedClientRequestReturnsErrorResponse`, `EmptyRequestBodyReturnsErrorResponse`, `UnknownRequestTypeReturnsErrorResponse`, `RoundTripSurvivesDatagramDuplication`, `IdenticalRequestReplayIsIdempotentEcho`) | Nested payload / fuzz later |
| **L-FORK-CHOICE** | Competing slot blocks / reorg | — | `non-goal` until fork choice ships |
| **L-MULTI-BEACON** | Inter-beacon sync / BFT | — | `non-goal` (see [SERVER.md](../architecture/SERVER.md) future) |

---

## Multi-process smoke scripts

| Script | Role |
|--------|------|
| `scripts/test/pp_ledger_local_test.sh` | Suite driver (`unit` / `l0` / `l1` / `latejoin` / `smoke`[=l0] / `image`) |
| `scripts/test/pp_ledger_network.sh` | Lifecycle `up` / `stop` / `clear` / `status` |
| `scripts/test/pp_ledger_smoke_lib.sh` | Shared bring-up + hard-fail `pp-client` helpers |
| `scripts/test/fixtures/*.tmpl` | Amp-aligned config templates |
| `scripts/test/pp_ledger_l0_smoke.sh` | L-SMOKE-L0 (L0a→L0b→L0c) |
| `scripts/test/pp_ledger_l1_smoke.sh` | L-SMOKE-L1 |
| `scripts/test/pp_ledger_latejoin_smoke.sh` | L-SMOKE-LATEJOIN |
| `test-network.sh` / `test-checkpoint-cycles.sh` | Thin root wrappers |
| `deploy/compose.smoke.yml` | Image smoke scaffold |

When adding a scenario: give it a purpose ID, a hard assert, and a skip reason if
it stays manual.

---

## Fixture conventions

- Unique temp dirs under `std::filesystem::temp_directory_path()`.
- Destroy stores / mesh runtimes before wiping dirs (parent-only teardown).
- Prefer `TEST_F` + `std::unique_ptr` over stack locals that outlive `remove_all`.
- Multi-process smoke configs: render from `scripts/test/fixtures/`; do not hand-edit
  generated files under `$BUILD_DIR/test-smoke/`.
