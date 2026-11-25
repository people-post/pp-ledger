# Releasing pp-ledger

**Tier:** ops

A release is the nodes' image and binaries (`pp-beacon`, `pp-relay`, `pp-miner`, `pp-client`, `pp-http`) and the
tag pp-browser (pp-node's ledger role) and pp-ledger-apps pin. The first is **2.0.0**
([RL004](../../projects/release/DECISIONS.md#rl004--the-first-release-is-200)). The `release/v1.0.0` … `release/v1.8.1`
tags before it were development pins, cut on `main`, and are no longer taken by the release workflow.

The design and its decisions: [projects/release](../../projects/release/DESIGN.md). The same model as pp-browser's
pp-node train ([pp-browser RELEASE.md](https://github.com/people-post/pp-browser/blob/main/docs/ops/RELEASE.md)).

## Branches

| Branch | Role |
|--------|------|
| **`main`** (default) | The integration line: every PR lands here ([build CI](../../.github/workflows/build-project.yml)). No release tag goes on main |
| **`release/X.Y`** | One per minor release, cut from main; takes `vX.Y.*` tags; open while a network runs it |

```text
main ──cut──► release/2.0 ──tag──► v2.0.0-rc.1 … v2.0.0-rc.<n> ──tag (same commit)──► v2.0.0
                         └─backport──► v2.0.1-rc.1 ──► v2.0.1
```

A release branch takes fixes only; fixes land on main first and are backported
([§ Release branches and backports](#release-branches-and-backports)). Release branches are never merged back.

## Tags

Release tags are pushed by the release owner (repo variable `RELEASE_OWNER`,
[RL008](../../projects/release/DECISIONS.md#rl008--hongwei-is-the-release-owner)) and are immutable: never move or
re-push one; a fix is a new candidate. A ruleset lets only a repository admin create, move or delete a `v*` tag; another
makes `release/**` take changes only by PR, with one approval and the build and format checks, as main.
[`release.yml`](../../.github/workflows/release.yml) refuses any other tag form, and any tag whose commit is not on
its release branch (same X.Y).

| Tag | On | What happens |
|-----|----|--------------|
| `vX.Y.Z-rc.<n>` | `release/X.Y` | Release candidate: the image built once, pushed as `ghcr.io/people-post/pp-ledger:X.Y.Z-rc.<n>`, the image smoke (L-SMOKE-IMAGE) run on it, build provenance attested, GitHub prerelease with the linux-x64 tarball |
| `vX.Y.Z` | the commit of a `vX.Y.Z-rc.<n>` | Release: **no build**. The candidate's image gets `X.Y.Z` (and `latest` when it is the newest `vX.Y.Z`; a fix to an older line does not move it); the GitHub release gets the candidate's tarball, its SHA-256 and the image digest |

`-rc.<n>` counts from 1 within a version. The guard refuses `vX.Y.Z-rc.<n>` once `vX.Y.Z` exists.

**Dev images:** Actions → **release** → Run workflow with a ref on main (refused otherwise). It pushes
`pp-ledger:0.0.0-dev.<sha>` and runs the image smoke. No tag, no GitHub release, never `latest`.

**Pins** ([RL005](../../projects/release/DECISIONS.md#rl005--consumers-pin-candidates-on-main-releases-on-release-branches)):
pp-browser's and pp-ledger-apps' `main` may pin a candidate (`vX.Y.Z-rc.<n>`) or a release; their release branches
pin releases only.

## Maintainer flow

1. Land work on **`main`** (PRs; build CI green).
2. Cut the release branch once per X.Y: `git push origin main:refs/heads/release/2.0`.
   [`release-issue.yml`](../../.github/workflows/release-issue.yml) opens the issue "Release pp-ledger 2.0" from
   [the template](../../.github/ISSUE_TEMPLATE/release.md), assigned to whoever pushed the branch, and creates the
   label `backport release/2.0`. Tick its checklist there; the go / no-go is recorded there.
3. Tag a candidate on the branch:

   ```bash
   git fetch origin && git checkout --detach origin/release/2.0
   git tag -a v2.0.0-rc.1 -m "pp-ledger 2.0.0 rc.1"
   git push origin v2.0.0-rc.1
   ```

4. Test the candidate (the release issue's list). A fix lands on main, is backported, and gets the next candidate.
5. Release the candidate that passed by tagging **its commit**:

   ```bash
   git tag -a v2.0.0 <commit of v2.0.0-rc.N> -m "pp-ledger 2.0.0" && git push origin v2.0.0
   ```

6. Move the pins in pp-browser and pp-ledger-apps to `v2.0.0`; tell operators (the release notes: what changed, the
   upgrade order, any activation epoch).

A hotfix is the same from the affected branch: backport to `release/2.0`, `v2.0.1-rc.1`, then `v2.0.1` on its
commit.

## What a release may change

Until a network runs on a release, the chain format may change freely (today's rule:
[WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md) — no backward compatibility, a new network). From the network that
launches on 2.0.0 on ([RL006](../../projects/release/DECISIONS.md#rl006--a-live-chain-is-read-forever-rules-change-at-a-signed-epoch)):

| Change | Takes |
|--------|-------|
| Code only (no bytes on the chain, the wire or a node's files change) | A patch or minor release; operators upgrade when they choose |
| A new readable form (a new RPC field, a new node file version) | A minor release that still reads the old form; the old one is written until every supported release reads the new |
| A consensus change (block or record layout, validation, the committee, fees) | A release that knows the old rules and the new, the new switched on at an epoch the genesis quorum signs after operators upgraded. The mechanism is not built yet ([projects/release PHASES § R3](../../projects/release/PHASES.md#r3--rule-activation)) |
| A break nothing can bridge | A new network ([RE_GENESIS.md](RE_GENESIS.md)): the emergency path, never the upgrade path |

Every block a network made stays readable and verifiable by every later release: a node joining from genesis
replays it with the rules each block was made under.

## Version table

What each release writes and reads. A row changes in the PR that changes the format; the release issue checks it.

| Format | Where | 2.0 writes | 2.0 reads |
|--------|-------|-----------|-----------|
| Block header (`Ledger::Block::CURRENT_VERSION`) | [WIRE_SCHEMA](../contracts/WIRE_SCHEMA.md) | 10 | 10 |
| Genesis meta (`GenesisAccountMeta::VERSION`) | [WIRE_SCHEMA](../contracts/WIRE_SCHEMA.md) | 15 | 15 |
| Account (`UserAccount`, account leaf v3) | [WIRE_SCHEMA](../contracts/WIRE_SCHEMA.md) | 5 | 5 |
| Beacon certificate (`T_BEACON_CERT`) | [BEACON_AUTHORITY](../contracts/BEACON_AUTHORITY.md) | 3 | 3 |
| Ledger RPC protocols | [AMP_TRANSPORT](../contracts/AMP_TRANSPORT.md) | `/pp-ledger/rpc/1.0.0`, `/pp-ledger/rpc-bulk/1.0.0` | the same |
| RPC envelope (`Request` / `Response`) | [AMP_TRANSPORT](../contracts/AMP_TRANSPORT.md) | 2 | 1, 2 |
| Amp wire, addresses | pp-cpp-amp | 2, `/adp/2/` | 2, `/adp/2/` |
| Node files (`SignedTip`, `RewindReport`, `Equivocation`) | [LEDGER_STORAGE](../contracts/LEDGER_STORAGE.md) | 1 | 1 |
| Operation file | [OPERATION_FILE](../contracts/OPERATION_FILE.md) | 1 | 1 |
| Key backup | [KEY_BACKUP](../contracts/KEY_BACKUP.md) | 1 | 1 |

Networks:

| Network | Network id | Genesis block hash | Runs | Launched |
|---------|------------|--------------------|------|----------|
| — | | | | Not yet ([projects/release PHASES § R4](../../projects/release/PHASES.md#r4--network-launch)) |

## Release branches and backports

The same rules and bots as pp-browser's ([its RELEASE.md § Release branches and
backports](https://github.com/people-post/pp-browser/blob/main/docs/ops/RELEASE.md#release-branches-and-backports)),
for `release/X.Y`:

- **What a branch takes:** fixes only. No features and no format changes; an additive change only where a fix needs
  it. The newest branch takes every fix; an older branch a network still runs takes security fixes and fixes for
  that network.
- **Backport:** merge the fix to main, label the PR `backport release/X.Y` (before or after the merge).
  [`backport.yml`](../../.github/workflows/backport.yml) cherry-picks its commits with `-x` onto
  `backport/pr-<N>-to-release-X.Y`, opens a PR to the branch linking the original, asks the release owner (repo
  variable `RELEASE_OWNER`; the author when unset) for review, and dispatches build CI on it. A conflict is reported
  on the original PR with the commands to do it by hand; the bot never resolves one. Whoever does says what differs
  on a line `branch-specific backport: …` in the commit message. A backport touching `.github/workflows/` is done by
  hand (the workflow token cannot push it).
- **A fix only a release branch needs:** a PR straight to the branch with the label `release-branch-only` and a line
  `Why not on main: …`. [`release-branch-pr.yml`](../../.github/workflows/release-branch-pr.yml) checks every PR to a
  release branch: that, or every commit's cherry-pick trailer naming a commit on main (or a `branch-specific
  backport` line).
- **Closing a branch** (no network runs it any more): close its open backport PRs, rename the label to
  `closed release/X.Y`, lock the branch in branch protection. The branch and its tags are kept.

## Checklist before tagging

### Candidate (`vX.Y.Z-rc.<n>`)

- [ ] Fixes on **main** first, backported to **`release/X.Y`**; the commit is on it (the guard refuses it otherwise)
- [ ] `release/X.Y` build CI green
- [ ] The version table row for X.Y matches what the branch writes and reads

### Release (`vX.Y.Z`)

- [ ] The candidate that passed is on **this commit** and its GitHub prerelease exists
- [ ] The release issue's go is recorded
