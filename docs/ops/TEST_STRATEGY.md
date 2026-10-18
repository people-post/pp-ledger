# Test strategy (purpose catalog, CI ladder)

**Tier:** ops

Purpose IDs (`L-*`) are the vocabulary for ledger qualification. Doctrine:
[TESTING.md](../architecture/TESTING.md).

Related: [AGENTS.md](../../AGENTS.md), [AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md),
[development budget plan](../../projects/development-budget/PLAN.md) (Phase A: adversarial
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
./scripts/test/pp_ledger_local_test.sh run --suite smoke --down   # l0 + l1 + latejoin, one network
./scripts/test/pp_ledger_local_test.sh run --suite image          # image + compose stack (docker)
./scripts/test/pp_ledger_local_test.sh run --suite burst          # goal 2 of projects/load-resilience
./scripts/test/pp_ledger_local_test.sh run --suite overload       # goal 1: floods on each role (~11 min)
./scripts/test/pp_ledger_local_test.sh run --suite upstream       # a relay whose beacon is frozen, then gone
./scripts/test/pp_ledger_local_test.sh run --suite scale          # the data axis, in-process (~1 min)
./scripts/test/pp_ledger_local_test.sh run --suite topology       # fan-out, load into slots, churn (~4 min)
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

**`--suite smoke`:** runs **l0 → l1 → latejoin** on one network, brought up once
(about 40–50 s in total). Run a single suite with `--suite l0|l1|latejoin`; without
`--down` the network stays up and the next run reuses it.

**Startup waits:** nodes are started without fixed grace sleeps. The driver
polls each node's console log until its run loop is up (`READY_MARKER`:
"Run loop started", logged after `onStart` — relay sync, miner
dial / register / startup sync) and fails at once if the process exits. The
three miners start in parallel. RPC / tip waits poll every 0.5 s.

Multi-process OsUdp dial requires `LedgerAmpRuntime` to call `SetAcceptEnabled(true)`
(same as pp-browser `MeshHost`). In-process Amp memory fabric gtests still own RPC
correctness under loss/reorder (`L-NET-*`). L1/LATEJOIN are deterministic without forced
leaders: registered miners, the delivery window and heartbeats keep the tip moving
([WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md)).

---

## CI ladder (target)

| Gate | Contents | Status |
|------|----------|--------|
| **PR** | `ctest` (unit + Amp RPC compose) | Wired today via `scripts/ci-build.sh --with-tests` |
| **Nightly / manual** | `--suite l0` / `l1` / `latejoin` via `pp_ledger_local_test.sh` | L0 / L1 / LATEJOIN green locally (`--suite smoke`, 10/10); **not CI-gated** yet |
| **Weekly / manual** | Chaos restart, soak, capacity curves | Planned (`cost/flake` until green) |
| **Image** | `deploy/compose.yml` + `deploy/init.sh` via `--suite image` | Needs docker; builds the image unless `PP_LEDGER_IMAGE` is set |

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
| **L-SMOKE-L0** | Binaries boot; client status reaches beacon; every node reports ready | Smoke (`scripts/test/pp_ledger_l0_smoke.sh`) | L0a/b/c fail-fast; L0d: `pp-client --host-file <node dir>/listen.multiaddr health --ready` passes for each node (nodes have no HTTP); green on localhost OsUdp |
| **L-COMPOSE-TIP** | Forced-leader block accepted by peer tip | Integration (`ForcedLeader_ProducerAndPeerAcceptTip`) | In-process stand-in for L-SMOKE-L1 |
| **L-SMOKE-L1** | Beacon→relay→miner produces tip | Smoke (`scripts/test/pp_ledger_l1_smoke.sh`) | Short slots + tx inject; a miner registers; the relay serves `CONFIG_GET` (`pp-client config`: the admin keys); then L-SMOKE-OPERATION, L-SMOKE-BATCH, L-SMOKE-PROOF and L-SMOKE-TOKEN; green |
| **L-OPERATION-FILE** | Operation files read back as written, merge copies signed apart, sign only by their signers and once per key, refuse tampering, a false kind, different operations to merge and the wrong signer account, summarize from the record; a genesis signed record by record, more records in a genesis only | Unit (`test_operation`) | [OPERATION_FILE.md](../contracts/OPERATION_FILE.md) |
| **L-GENESIS-OFFLINE** | Block 0 planned by the beacon, signed apart by the admin keys and merged, starts the chain at its planned time; refused short of its quorum or certifying another beacon key | Unit (`AGenesisSignedOfflineStartsTheChainAtItsPlannedTime`, `AnOfflineGenesisNeedsItsQuorumAndThisBeaconsKey`) + every smoke bring-up (`--plan-genesis` → `op sign` ×3 → `op merge` → `--init --genesis-op`; a re-genesis keeps `--genesis-key`) | [OPERATION_FILE.md § Genesis](../contracts/OPERATION_FILE.md#genesis) |
| **L-SMOKE-OPERATION** | A multi-signature operation made from the chain, signed apart by the three reserve holders, merged and submitted, lands | Smoke (L1 suite, `run_operation_file_scenario`) | `op new-account` read from the relay → `op sign` ×3 (copies) → `op submit` of one copy refused (short of quorum) → `op merge` → `op submit` → the account exists; green |
| **L-SMOKE-BATCH** | A batch from a multi-signature account; producer statistics on a live chain | Smoke (L1 suite, `run_batch_scenario`) | `op batch` from the reserve (CSV: the registrar 700, carol 300, with metadata) → `op sign` ×3 → `op merge` → `op submit` (transfer by transfer) → both paid; `pp-client producers --from 1`: the producers' blocks add up to the range's, fees counted; green |
| **L-SMOKE-PROOF** | A wallet's verified read on a live chain | Smoke (L1 suite, `run_verified_account_scenario`) | `pp-client verified-account` for carol with block 0's hash: authority proof, signed tip, blocks, account proof — her balance as the node shows it; an absent account proven absent; a wrong pinned hash refused; green |
| **L-SMOKE-TOKEN** | A token issuer: made by the registrar, issues its own token, replaces its keys | Smoke (L1 suite, `run_token_scenario`) | Reserve → registrar (3 of 3) → registrar creates an issued account (3 of 3; the first id not yet on the chain, so a network kept up across runs works) → it sends 1,000 of its token to carol: carol holds 1,000, the issuer −1,000 → `op update-keys` to 2 of 2; green |
| **L-SMOKE-RECOVERY** | An account's recovery set gives it new keys; its owner can cancel | Smoke (L1 suite, `run_account_recovery_scenario`) | A new account (reserve, 3 of 3) → `op set-recovery` (2 guardians, both to sign, 1 epoch) → one guardian alone cannot announce → `op recover` (both) pending → `op cancel-recovery` (the owner) clears it → `op recover` again applies at the epoch after next: the account's key is the announced one, which signs a transfer; the old key's transfer never lands; green |
| **L-SMOKE-LATEJOIN** | Late miner tip catches beacon tip | Smoke (`scripts/test/pp_ledger_latejoin_smoke.sh`) | Hard `nextBlockId` assert; green |
| **L-SMOKE-ROTATION** | Beacon key rotation: generation 2 signs, miners follow; fresh miners join at a checkpoint above 0; the recovery set replaces the admin set; equivocation evidence halts the network until a later generation; a certificate out of band rewinds every node to its cutover | Smoke (`scripts/test/pp_ledger_rotation_smoke.sh`, `--suite rotation`) | Own network (3-slot epochs; next key staged before start; relay pins genesis); `mk-beacon-cert` (no cutover) → `sign-tx` ×3 → `submit-tx`; then a miner pinning genesis (checked by `AUTHORITY_PROOF`, which carries the certificate) and one not restart empty at a checkpoint and validate Full past the next one. Last (L-SMOKE-REWIND, ba5c / bp1): a generation-3 certificate for a key nobody holds, cutover two blocks below the tip, submitted to the relay — the relay, the beacon and the miners holding blocks past the cutover each rewind to it by restarting in-process (no process exits), and the miner that was down during the cut rewinds once back online, the relay reports the rewind and still serves the certificate, the beacon stands down, nothing commits. First (L-SMOKE-RECOVERY, ba7): `mk-recovery` for three new admin keys, signed by the recovery key and submitted to the relay — STATUS shows it pending, the old admins' certificate is refused, and at its epoch the new set takes over (the rest of the suite signs with it). Then (L-SMOKE-EQUIVOCATION, ba6): `mk-equivocation` with the generation-1 key at the tip, `add-equivocation` to the relay — relay, beacon and every miner keep it, STATUS serves it, nothing commits; the generation-2 certificate resumes the network and the evidence files go; green |
| **L-SMOKE-TAKEOVER** | Case 2 end to end: a stolen beacon key is replaced by a relay promoted to be the new beacon, and the network resumes under the new generation | Smoke (`scripts/test/pp_ledger_takeover_smoke.sh`, `--suite takeover`) | Own network: beacon, relay1 → 3 miners, relay2 and relay3 (following relay2). A key generated on relay2 is certified as generation 2 with a cutover two blocks below the tip and relay3 as dial hint, submitted to relay1 and relay2 once every node holds the tip; every node rewinds, the old beacon stands down; relay2 is stopped and restarted with `pp-beacon --promote` (clock checked, `config.relay.json` kept); a leader commits the certificate in the block after the cutover, the new beacon signs it, relay1 and the miners move to relay3 and follow generation 2, and the miners recalibrate their clocks; green |
| **L-SMOKE-MOVE** | Case 3: the beacon moves to another host with its key and generation (runbook [MOVING_THE_BEACON.md](MOVING_THE_BEACON.md)) | Smoke (`scripts/test/pp_ledger_move_smoke.sh`, `--suite move`) | Own network: beacon, relay → 3 miners; the relay lists the beacon's new address (same peer id, another port) as a second candidate. The beacon stops on SIGTERM (exit 0), its work directory is copied whole and its old signing key removed; a copy without `data/signed-tip.dat` refuses to start (`--accept-ledger-tip`); the copy starts on the new port from the tip it signed, the relay follows it, and the relay and miners go on under generation 1 with no rewind and no equivocation; green |
| **L-SMOKE-REGENESIS** | Case 1: a new network imports the old one's state; nodes that ran the old network check it (runbook [RE_GENESIS.md](RE_GENESIS.md)) | Smoke (`scripts/test/pp_ledger_regenesis_smoke.sh`, `--suite regenesis`) | Two networks in turn: beacon, relay → 3 miners with accounts; the beacon stops, every node holds the last block *H*, all stop; `export-state` from the relay at *H*; a new network (new id and keys) `--init --import-state`; a fresh relay and miners with `networkAnchor.predecessorDir` = their old directories each log the predecessor checked; an account is unchanged; `check-predecessor` passes against an old miner's ledger and refuses another network's; the new network grows with the old miners' keys; green |
| **L-SMOKE-IMAGE** | The image and sample stack run from the environment: healthy (`pp-client health`), ready, pp-http `/metrics` reports every node ready, the chain grows, the HTTP API answers, every node got its 8 MiB UDP buffers past the host's cap (`CAP_NET_ADMIN`), a clean stop | Smoke (`--suite image`: `deploy/init.sh`, `docker compose up --wait`) | Own project, subnet (`172.29.7`) and host ports (285xx, 28080); green |
| **L-LOAD-JUNK** | Badly signed transfers never take a place in the pool; valid ones beside them are taken and included | Smoke (`scripts/test/pp_ledger_burst_smoke.sh`, `--suite burst`) | 20/s for 15 s, one in ten junk, through the relay (`pp-client load`); junk refused at the relay, none in a block, every valid transfer within three blocks; green |
| **L-LOAD-BURST** | A burst several blocks deep from many new clients is absorbed: taken whole, included within about the blocks it fills, no client told a failure for a transfer that went through | Smoke (`--suite burst`) | Own network, renewals once in a long while (projects/load-resilience LR004); 500 transfers at once from 64 new client identities on 10/s for 20 s; checks `taken == sent == included`, `includedNotTaken == 0`, drained within ⌈700 ÷ 100⌉ + 3 blocks; green |
| **L-LOAD-OVERLOAD** | Goal 1 (survive overload): floods on the relay, the beacon and a miner leave each alive and bounded, the chain's duties going on, the excess answered fast, everyone else served | Smoke (`scripts/test/pp_ledger_overload_smoke.sh`, `--suite overload`) | Own network; fifteen floods of 8 s, each with 10 transfers/s beside (through the relay) and the flooded node's `HEALTH` probed every 0.5 s. On the relay: `STATUS` 8,000/s, a herd of 400 new clients, wallet-history scans 2,000/s, forged transfers 4,000/s, forged registrations 6,000/s, parked `BLOCK_WAIT`s from 400 clients, 4 MiB bodies 64/s, valid transfers 2,000/s past the pool; on the beacon reads, forged registrations, 4 MiB bodies; on a miner reads, forged transfers, 4 MiB bodies, valid transfers it forwards up. Each: no request sent left unanswered, flood replies p99 within a bound (2 s; parked waits 8 s, large bodies 15 s), a herd's dials at most 2 % failed, `HEALTH` within 2 s every time, the transfers beside taken (or told the pool is full) and included with replies p99 ≤ 2 s, the chain grew and no miner's block was lost, the flooded node ready and under 512 MiB after. `PP_LEDGER_OVERLOAD_ONLY="herd wait"` runs some floods; about 11 minutes; green |
| **L-LOAD-UPSTREAM** | A relay whose upstream is slow or gone answers for it and recovers | Smoke (`scripts/test/pp_ledger_upstream_smoke.sh`, `--suite upstream`) | Own network; the beacon frozen (SIGSTOP) 10 s, then stopped and restarted; transfers through the relay with a 6 s deadline (`load --timeout`) and reads beside: every transfer answered before its deadline ("busy" / "the upstream did not answer in time"), none taken, the relay's reads and `HEALTH` served, then the relay syncs again and transfers are taken and included; about 2 minutes; green |
| **L-LOAD-SCALE** | The data axis: what grows with the chain — sync from genesis, a restart's replay, wallet history — stays linear and cheap | In-process (`test_ledger_scale`, `--suite scale`; skipped unless `PP_LEDGER_SCALE=1`) | A chain of real signed transfers built on virtual time (`PP_LEDGER_SCALE_ACCOUNTS` 1,000, `_BLOCKS` 1,000, `_TXS` 100 a block); reports tx/s synced and replayed and what fits in `startupSyncTimeout`; checks a restart replays at least half as fast as a sync and a wallet-history request stays under 50 ms; JSON in `build/ledger-scale.json`; about a minute. Beside it, unit tests: a replay reads the ledger in linear time (`ReplayReadsTheLedgerInLinearTime`), a startup sync that progresses is not cut off (`ALongSyncThatProgressesIsNotCutOff`), the renewal echo is as long as the burst (`RenewalEchoTest`, an in-process miner, ~20 s); green |
| **L-LOAD-TOPOLOGY** | What only shows between nodes holds: start-up, fan-out, load into slots, churn | Smoke (`scripts/test/pp_ledger_topology_smoke.sh`, `--suite topology`) | Own network. Transfers from the first slots: none refused late, none lost. Relays added behind the beacon (1, 5, 9): every relay within 3 blocks, the beacon's run-loop turns under 1 s. Transfers through the relay at 20/s, then 40/s with 4,000 reads/s: taken, included within 3 blocks, at least 0.8 of the slots with their block, no block lost. The miners' relay stopped 10 s: every miner registers again, transfers go through; each miner stopped 8 s in turn: the chain goes on, every transfer included; about 3–4 minutes; green |
| **L-SMOKE-CHAOS** | Restart relay/miner; recover or fail clean | Smoke | Covered by L-LOAD-TOPOLOGY (churn: the miners' relay restarted, each miner restarted in turn) |
| **L-ADV-INGRESS** | Malformed / oversize / replay at RPC ingress | Integration (`ClientRejectsOversizeRequest`, `TruncatedClientRequestReturnsErrorResponse`, `EmptyRequestBodyReturnsErrorResponse`, `UnknownRequestTypeReturnsErrorResponse`, `RoundTripSurvivesDatagramDuplication`, `IdenticalRequestReplayIsIdempotentEcho`) | Nested payload / fuzz later |
| **L-FORK-CHOICE** | Competing slot blocks / reorg | — | `non-goal` until fork choice ships |
| **L-MULTI-BEACON** | Inter-beacon sync / BFT | — | `non-goal` (see [SERVER.md](../architecture/SERVER.md) future) |

---

## Multi-process smoke scripts

| Script | Role |
|--------|------|
| `scripts/test/pp_ledger_local_test.sh` | Suite driver (`unit` / `l0` / `l1` / `latejoin` / `smoke`[=l0] / `rotation` / `takeover` / `move` / `regenesis` / `burst` / `image`) |
| `scripts/test/pp_ledger_network.sh` | Lifecycle `up` / `stop` / `clear` / `status` |
| `scripts/test/pp_ledger_smoke_lib.sh` | Shared bring-up + hard-fail `pp-client` helpers |
| `scripts/test/fixtures/*.tmpl` | Amp-aligned config templates |
| `scripts/test/pp_ledger_l0_smoke.sh` | L-SMOKE-L0 (L0a→L0b→L0c) |
| `scripts/test/pp_ledger_l1_smoke.sh` | L-SMOKE-L1 |
| `scripts/test/pp_ledger_latejoin_smoke.sh` | L-SMOKE-LATEJOIN |
| `scripts/test/pp_ledger_rotation_smoke.sh` | L-SMOKE-ROTATION |
| `scripts/test/pp_ledger_takeover_smoke.sh` | L-SMOKE-TAKEOVER |
| `scripts/test/pp_ledger_move_smoke.sh` | L-SMOKE-MOVE |
| `scripts/test/pp_ledger_regenesis_smoke.sh` | L-SMOKE-REGENESIS |
| `scripts/test/pp_ledger_burst_smoke.sh` | L-LOAD-JUNK, L-LOAD-BURST |
| `scripts/test/pp_ledger_overload_smoke.sh` | L-LOAD-OVERLOAD |
| `scripts/test/pp_ledger_upstream_smoke.sh` | L-LOAD-UPSTREAM |
| `scripts/test/pp_ledger_topology_smoke.sh` | L-LOAD-TOPOLOGY |
| `test-network.sh` / `test-checkpoint-cycles.sh` | Thin root wrappers |
| `deploy/compose.yml`, `deploy/init.sh` | Sample stack; the image smoke (`--suite image`) runs it |

When adding a scenario: give it a purpose ID, a hard assert, and a skip reason if
it stays manual.

## Load generator

`pp-client load` (projects/load-resilience): requests at a fixed rate whatever the replies, from
`--peers` client identities, every reply classified (`taken`, `duplicate`, `pool_full`, `served`,
`busy`, `expired`, `timeout` — sent and not answered —, `unsent` — never sent: no link, no channel, no
place to the peer —, `refused`, with the commonest error messages), transfers followed into blocks
(inclusion seconds and blocks; transfers included although their reply was an error; junk that must
never be included) for `--drain` seconds at most, or until none was for `--quiet`. `--timeout` sets
each request's deadline (milliseconds). `--json` writes the report for scripts.

```bash
R=$(cat build/test-smoke/multiaddrs/relay); K=build/test-smoke/keys
# Funded accounts, once per network (the reserve signs: three keys in the smoke fixture)
pp-client -b --host "$R" load-setup --from 1 -k $K/reserve1.key -k $K/reserve2.key -k $K/reserve3.key \
  -n 200 --amount 100000 -o accounts.json
# 20/s for 15 s, 10 % junk; then a burst of 500 transfers two seconds in
pp-client -b --host "$R" load --accounts accounts.json --mix tx_add=9,tx_junk=1 -r 20 -d 15
pp-client -b --host "$R" load --accounts accounts.json -r 10 -d 20 --burst 500 --burst-at 2 --peers 64
```

Mix kinds: `tx_add`, `tx_junk` (decodes, badly signed), `status`, `account_get`, `block_get`,
`tx_get_by_wallet`, `big` (a ~4 MiB `BLOCK_ADD` that is no block: received whole, then refused),
`block_wait` (parked until the next block), `register` (a funded account's miner registration,
badly signed: a relay forwards it, the beacon refuses it). The smoke fixture renews every account every two blocks
(`checkpointMinBlocks` 2) with 100 records a block: more than a block's worth of accounts makes
every block renewals-only. Start a load network with `PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS=1000`.

---

## Fixture conventions

- Temp paths come from `pp::test::uniqueTempPath(name)` (`src/testing/TestTempPath.h`, target
  `pp_testing`), never a fixed name under `temp_directory_path()`: ctest runs each test case in its
  own process and `ctest -j` runs them side by side, so a fixed name is shared (one test wipes or
  reopens another's ledger). The helper appends the process id. Remove what you create — the name
  is new on every run, so nothing overwrites a leftover.
- Destroy stores / mesh runtimes before wiping dirs (parent-only teardown).
- Prefer `TEST_F` + `std::unique_ptr` over stack locals that outlive `remove_all`.
- Multi-process smoke configs: render from `scripts/test/fixtures/`; do not hand-edit
  generated files under `$BUILD_DIR/test-smoke/`.
