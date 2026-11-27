# Release — phases

Order of work only; status in [CURRENT_STATE.md](CURRENT_STATE.md).

## R1 — Release mechanics

- [x] `release.yml`: guard, candidate (image built once, image smoke, provenance, prerelease), promote, dev images
- [x] Build CI on `release/**` and PRs to it; backport bot, release-branch PR check, release issue and its template
- [x] [docs/ops/RELEASE.md](../../docs/ops/RELEASE.md) with the version table
- [x] The `RELEASE_OWNER` variable (Hongwei, RL008)
- [x] Rulesets: `release/**` as main (a PR with one approval, the build and format checks, no force-push or deletion);
      `v*` tags created, moved or deleted only by a repository admin
- [ ] The GHCR package's visibility (public, if anyone outside pulls it)
- [x] pp-browser: the release-compatibility docs say pp-ledger has release branches (RC023, people-post/pp-browser#773)

## R2 — Compatibility tests

- [ ] Build identity: each node says its release and commit (`STATUS`, `pp-client health`, pp-http `/metrics`), so
      operators and the mixed-version tests see who runs what
- [ ] Frozen chains: a chain made by each release (blocks, a checkpoint, the node files) kept in the repo; every later
      release loads, replays and verifies it in ctest. The first is made by 2.0.0's candidate
- [ ] Mixed versions: a smoke with the last release's image and the candidate's binaries in one network (relay and
      miners new, beacon old, and the reverse); in the release issue's list from 2.1 on

## R3 — Rule activation

Its own project (design first): a signed record naming new rules and their epoch, nodes that refuse to pass an
activation they do not know, replay by each block's rules, and the smokes. Before 2.0.0, which the testnet launches
on ([RL006](DECISIONS.md#rl006--a-live-chain-is-read-forever-rules-change-at-a-signed-epoch),
[RL010](DECISIONS.md#rl010--the-testnet-is-live-the-freeze-starts-at-its-launch)).

## R4 — Testnet launch

The networks: [RL009](DECISIONS.md#rl009--three-networks-devnet-testnet-then-mainnet-long-after).

- [ ] Runbook `docs/ops/NETWORK_LAUNCH.md`: the genesis key ceremony and its holders, `init-config.json`, the beacon's
      init, the first relays and miners, publishing the network id and genesis block hash; the same for both networks
- [ ] Rehearsed on the devnet, from the 2.0.0 candidate
- [ ] Hosting of the devnet and the public networks: infrastructure's (Leo), in a private repository
- [ ] The testnet's onboarding account and how users ask for tokens
      ([RL012](DECISIONS.md#rl012--testnet-tokens-by-hand-from-an-onboarding-account))
- [ ] The testnet's network card published; pp-ledger-apps and pp-node pin it

## R5 — 2.0.0

- [x] Miner producer keys: a miner host holds a producer key its account certifies, not the account's keys
      ([WIRE_SCHEMA § Miner producer keys](../../docs/contracts/WIRE_SCHEMA.md#miner-producer-keys))
- [ ] `release/2.0` cut once R2's build identity and frozen-chain tests, R3 and the producer keys are on main
- [ ] Candidates until the release issue says go; `v2.0.0`
- [ ] The testnet launched on it (R4); pins in pp-browser and pp-ledger-apps moved to `v2.0.0`
- [ ] WIRE_SCHEMA's "no backward compatibility" replaced by RELEASE.md § What a release may change

## R6 — Soak, then mainnet

- [ ] Every release on the testnet before mainnet; its release issue records how it ran there
- [ ] The go-ahead criteria met and recorded
      ([RL011](DECISIONS.md#rl011--mainnet-launches-when-the-testnet-met-its-go-ahead-criteria)): 3 months at least,
      an upgrade through rule activation, a producer key replacement and a beacon key rotation drilled, no consensus
      incident in 30 days, 3 outside operators, a load test, an external security review
- [ ] Mainnet launched (R4's runbook, its own keys and holders) on the release of the day; its network card
      published; pp-ledger-apps and pp-node pin it beside the testnet's
