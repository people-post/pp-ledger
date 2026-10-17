# Load resilience — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

## Measure

- [x] **lr0** Design: the two goals, their pass criteria, tiers (this folder)
- [x] **lr1** Metrics the checks read: request latency and queue wait per type, "busy" and expired
      counts, duty lateness (slot end → seal → commit), pool size and oldest age — on `HEALTH` and
      pp-http `/metrics`
- [x] **lr2** Load generator `pp-client load` (open loop, request mixes, many identities, signed
      transactions, outcome and inclusion reporting) and a funded-accounts genesis fixture
- [ ] **lr3** Burst baseline on the smoke network (short slots): goal 2's checks with valid
      transactions, then with invalid ones mixed in; record what fails (expected: DESIGN § Where we
      start)

## Absorb bursts (goal 2)

- [x] **lr4** Pool: cheap admission checks; transactions a leader finds permanently invalid leave
      the pool; a share per sender; drain order and limits as unit tests
- [x] **lr5** The pull limit from the chain's config (records and bytes), not a constant
- [x] **lr6** Overload answers and retries (LR006, LR007): `E_BUSY` / `E_RATE_LIMITED` /
      `E_POOL_FULL` with retry-after; per-peer limit announced and queued for in `pp::Client`; one
      retry policy for transactions (same bytes); the request's deadline forwarded hop by hop (a
      relay's forward at half the client's timeout until then); `pp-client add-tx` resends instead
      of re-signing
- [ ] **lr6b** Links like TCP (LR005), in pp-cpp-amp then the ledger: configurable `max_links` and
      handshake backlog, silent drop with handshake resends, fast refusal of a full table after
      evicting the longest-idle inbound link, growing jittered dial backoff; ledger `maxLinks`,
      `handshakeBacklog` per role
- [ ] **lr7** Burst suite (`--suite burst`): goal 2's checks green, through relays; load fixture with
      a realistic renewal period (LR004)

## Survive overload (goal 1)

- [ ] **lr8** Requests axis — memory per link (sizes LR005's defaults); single-server overload per role, upstreams scripted (slow, refusing,
      absent): cheap and expensive reads, `TX_ADD` / `REGISTER` floods, parked `BLOCK_WAIT`s,
      oversized requests, new-identity (handshake) floods; goal 1's checks; fixes found on the way;
      small "fails cleanly" cases as PR-gating tests
- [ ] **lr9** Data axis — generated large ledgers: request cost on long chains, startup and sync
      from genesis within `startupSyncTimeout`, the renewal storm after a re-genesis, the renewal
      echo of a busy period (LR004)
- [ ] **lr10** Topology axis — network scenarios (`--suite overload`): fan-out ladder, steady
      transactions through relays, timeouts across hops, load into missed slots, churn (rolling
      restarts, a relay down while its miners lead, a rewind or certificate across the fleet),
      optional netem; nightly / manual

## Ship

- [ ] **lr11** Promote: limits and behaviour under load into [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md)
      and [SERVER.md](../../docs/architecture/SERVER.md); purposes (`L-LOAD-*`) and suites into
      [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md)
