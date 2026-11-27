# Release — design

How pp-ledger is released, and what a release may change once a network runs on it. Decisions:
[DECISIONS.md](DECISIONS.md); order of work: [PHASES.md](PHASES.md); status: [CURRENT_STATE.md](CURRENT_STATE.md). The
process as it runs: [docs/ops/RELEASE.md](../../docs/ops/RELEASE.md).

## What a release is

pp-ledger is two things at once:

| As | What ships | Who takes it |
|----|------------|--------------|
| **A network's software** | The image `ghcr.io/people-post/pp-ledger` and a linux-x64 tarball: `pp-beacon`, `pp-relay`, `pp-miner`, `pp-client`, `pp-http` | Operators: the beacon, relays, miners, and services on their own relay ([SERVICE_API](../../docs/contracts/SERVICE_API.md)) |
| **A library** | The git tag | pp-browser (pp-node's ledger role, `cmake/PpPins.cmake`), pp-ledger-apps (the wallet's verifier, `cmake/PpLedger.cmake`) |

Until now only the second was planned for: pp-browser's release-compatibility design gives pp-ledger, like the
pp-cpp-* libraries, `main` and tags only, and the tags (`release/v1.0.0` … `release/v1.8.1`) were cut on main whenever
a consumer needed a pin. A network's software needs more: operators need a fix without main's features, a release
must be the bytes that were tested, and a chain, once it runs, is a format that can never be rewritten.

## The model

pp-browser's pp-node train, unchanged where it fits ([RL001](DECISIONS.md#rl001--pp-ledger-releases-as-a-product-release-branches-and-candidates)):

- **`main` and `release/X.Y`.** One branch per minor, cut from main; fixes land on main and are backported by the
  same bot and checks as pp-browser's (`backport release/X.Y` labels, `release-branch-only` with its reason).
- **Candidates, then a release, built once** ([RL002](DECISIONS.md#rl002--tags-are-vxyz-rcn-and-vxyz),
  [RL003](DECISIONS.md#rl003--built-once-a-release-promotes-its-candidate)). `vX.Y.Z-rc.<n>` builds the image,
  smokes it, attests its provenance and publishes a prerelease; `vX.Y.Z` on the same commit re-tags that image (same
  digest) and releases that tarball. `latest` only for the newest release.
- **A release issue per branch** with the checklist and the go / no-go, kept by the release owner
  ([RL008](DECISIONS.md#rl008--hongwei-is-the-release-owner)), who also pushes the release tags.
- **The first release is 2.0.0** ([RL004](DECISIONS.md#rl004--the-first-release-is-200)).

What pp-ledger adds:

1. **Consumers' pins** ([RL005](DECISIONS.md#rl005--consumers-pin-candidates-on-main-releases-on-release-branches)).
   Their main may pin a candidate, so pp-node can take a ledger change without waiting for a release; their release
   branches pin releases. A pp-node release branch that needs a ledger fix gets it from a pp-ledger patch release on
   the pp-ledger branch it pins.
2. **A version table** ([docs/ops/RELEASE.md § Version table](../../docs/ops/RELEASE.md#version-table)): per release,
   the chain, protocol and file formats it writes and reads, and which networks run it. pp-node and pp-ledger-apps
   releases name the pp-ledger release they pin, so the table answers "which app reads which network".
3. **A live chain's rule** ([RL006](DECISIONS.md#rl006--a-live-chain-is-read-forever-rules-change-at-a-signed-epoch)).
4. **The network launch, apart from any release** ([RL007](DECISIONS.md#rl007--a-network-launch-is-not-a-release)),
   for the testnet first and mainnet long after ([RL009](DECISIONS.md#rl009--three-networks-devnet-testnet-then-mainnet-long-after)).

## A live chain

Today a format change means a new network ([WIRE_SCHEMA](../../docs/contracts/WIRE_SCHEMA.md): "no backward
compatibility with earlier block versions"; [RE_GENESIS](../../docs/ops/RE_GENESIS.md)). That holds until the testnet
launches on 2.0.0 ([RL010](DECISIONS.md#rl010--the-testnet-is-live-the-freeze-starts-at-its-launch)). From then, for it
and for mainnet after it:
- **Every block stays readable.** A node that joins from genesis replays every block with the rules it was made under,
  so each release keeps every earlier block and record version's reading and validation.
- **Consensus changes switch on at a signed epoch.** A release that knows the old and the new rules ships first;
  operators upgrade; the genesis quorum signs a record naming the epoch the new rules start (as `T_CONFIG` does for
  config, with its activation epoch). A node that does not know the named rules stops before that epoch, with a plain
  "update needed", rather than forking. Not built ([PHASES § R3](PHASES.md#r3--rule-activation)); it has to be in
  2.0.0, since a network without it can take its first rule change only by re-genesis.
- **Re-genesis stays the emergency path** (lost keys), never the upgrade path.
- **The wire and node files** follow the range rule pp-browser uses: a new form is read by a release before any
  release writes it.

Tested by frozen chains and mixed versions ([PHASES § R2](PHASES.md#r2--compatibility-tests)): a chain made by each
release, kept in the repo, must load and verify in every later one; a candidate's nodes run against the last
release's.

## Networks

Three ([RL009](DECISIONS.md#rl009--three-networks-devnet-testnet-then-mainnet-long-after)): a **devnet** for us, reset at
will, on dev images; a **testnet** for users and operators, launched first, on 2.0.0, and live from then on
([RL010](DECISIONS.md#rl010--the-testnet-is-live-the-freeze-starts-at-its-launch): the freeze, rule activation,
a reset only at a critical failure); **mainnet**, launched after a long testnet run, once its go-ahead criteria hold
([RL011](DECISIONS.md#rl011--mainnet-launches-when-the-testnet-met-its-go-ahead-criteria)). Every release runs on the
testnet before mainnet.

A client picks its ledger network by network card (network id and genesis hash), on its own: while the testnet is the
only public chain, a product's production setup runs on it.

## The network launch

Not a software release, and done twice — the testnet, then mainnet — each with its own keys: the genesis key ceremony
(genesis, recovery and admin keys made offline by their holders,
[deploy/README.md § Production notes](../../deploy/README.md#production-notes)), `init-config.json`, the beacon's
`--init`, and publishing the network's id and genesis block hash, the pin pp-ledger-apps and pp-node trust. Its
runbook is this repository's ([PHASES § R4](PHASES.md#r4--testnet-launch)), rehearsed on the devnet; where the nodes
run is infrastructure's (Leo's), and the hosts, accounts and deploys stay out of this public repository.

## Open

- How long an older `release/X.Y` stays open once no network runs it (today: closed then).
- Multi-arch images (`linux/arm64`), macOS / Windows binaries on the release: not now; CI builds them, nobody runs
  them as nodes yet.
