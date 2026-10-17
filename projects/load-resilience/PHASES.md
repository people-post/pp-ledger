# Load resilience — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

## Measure

- [x] **lr0** Design: the two goals, their pass criteria, tiers (this folder)
- [x] **lr1** Metrics the checks read: request latency and queue wait per type, "busy" and expired
      counts, duty lateness (slot end → seal → commit), pool size and oldest age — on `HEALTH` and
      pp-http `/metrics`
- [ ] **lr2** Load generator `pp-client load` (open loop, request mixes, many identities, signed
      transactions, outcome and inclusion reporting) and a funded-accounts genesis fixture
- [ ] **lr3** Burst baseline on the smoke network (short slots): goal 2's checks with valid
      transactions, then with invalid ones mixed in; record what fails (expected: DESIGN § Where we
      start)

## Absorb bursts (goal 2)

- [ ] **lr4** Pool: cheap admission checks; transactions a leader finds permanently invalid leave
      the pool; a share per sender; drain order and limits as unit tests
- [ ] **lr5** The pull limit from the chain's config (records and bytes), not a constant
- [ ] **lr6** Relay forwarding absorbs a burst: "retry" only when the pool is full
- [ ] **lr7** Burst suite (`--suite burst`): goal 2's checks green, through relays

## Survive overload (goal 1)

- [ ] **lr8** Requests axis — single-server overload per role, upstreams scripted (slow, refusing,
      absent): cheap and expensive reads, `TX_ADD` / `REGISTER` floods, parked `BLOCK_WAIT`s,
      oversized requests, new-identity (handshake) floods; goal 1's checks; fixes found on the way;
      small "fails cleanly" cases as PR-gating tests
- [ ] **lr9** Data axis — generated large ledgers: request cost on long chains, startup and sync
      from genesis within `startupSyncTimeout`, the renewal storm after a re-genesis
- [ ] **lr10** Topology axis — network scenarios (`--suite overload`): fan-out ladder, steady
      transactions through relays, timeouts across hops, load into missed slots, churn (rolling
      restarts, a relay down while its miners lead, a rewind or certificate across the fleet),
      optional netem; nightly / manual

## Ship

- [ ] **lr11** Promote: limits and behaviour under load into [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md)
      and [SERVER.md](../../docs/architecture/SERVER.md); purposes (`L-LOAD-*`) and suites into
      [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md)
