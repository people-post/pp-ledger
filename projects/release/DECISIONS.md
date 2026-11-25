# Release — decisions

Rationale for [DESIGN.md](DESIGN.md). When behaviour ships, promote it into `docs/` and mark the entry
**superseded by** that doc.

## RL001 — pp-ledger releases as a product: release branches and candidates

**Date:** 2026-11-24
**Status:** Accepted (Hongwei, 2026-11-24). Supersedes, for pp-ledger, pp-browser's "the libraries (pp-cpp-*) and
pp-ledger have only `main`" (pp-browser release-compatibility RC023).
**Superseded by:** [docs/ops/RELEASE.md](../../docs/ops/RELEASE.md) (the process; this entry keeps the why).
**Decision:** pp-ledger has `main` and one `release/X.Y` per minor release, with pp-browser's backport bot, its
release-branch PR check and a release issue per branch, as pp-browser's pp-node train.
**Rationale:** pp-ledger is a network's software, not only a library compiled into a product: operators run its
nodes and need a fix without taking main's features, and a chain's format is frozen once a network runs. The
machinery exists and works in pp-browser; copying it keeps one way of releasing across the repos.
**Alternatives:** Main plus tags, as the pp-cpp-* libraries (a fix to a running network would carry everything since
its release); a branch cut only when needed (the first fix is the worst moment to set it up).

## RL002 — Tags are vX.Y.Z-rc.N and vX.Y.Z

**Date:** 2026-11-24
**Status:** Accepted (Hongwei, 2026-11-24).
**Decision:** Candidates `vX.Y.Z-rc.<n>` and releases `vX.Y.Z`, on `release/X.Y` only. The `release/vX.Y.Z` form is
retired; its tags stay (immutable) and the release workflow no longer takes it.
**Rationale:** `release/…` names a branch in pp-browser and here from now on; a tag of the same shape is a trap. The
form matches pp-browser's app and pp-relay-apis.
**Alternatives:** Keep `release/vX.Y.Z` (clashes with the branch names).

## RL003 — Built once: a release promotes its candidate

**Date:** 2026-11-24
**Status:** Accepted (Hongwei, 2026-11-24).
**Decision:** A candidate builds the image once, runs the image smoke on the pushed image, attests its build
provenance and publishes a prerelease with the tarball. The release tag on the same commit re-tags that image
(`docker buildx imagetools create`, same digest) and releases that tarball with its SHA-256. `latest` moves only
for the newest `vX.Y.Z`. Dev images (`0.0.0-dev.<sha>`) are built by hand from main only, untagged.
**Rationale:** What operators run is exactly what was tested. The old workflow rebuilt on every tag and moved
`latest` on any tag, so a fix to an older line would have become `latest`.
**Alternatives:** Build on the release tag (a rebuild is not the tested bytes).

## RL004 — The first release is 2.0.0

**Date:** 2026-11-24
**Status:** Accepted (Hongwei, 2026-11-24).
**Decision:** The first real release is 2.0.0: the first a network launches on, and from which the chain format is
frozen ([RL006](#rl006--a-live-chain-is-read-forever-rules-change-at-a-signed-epoch)).
**Rationale:** Nothing has been released for real, so 1.0.0 was considered first; but `release/v1.0.0` …
`release/v1.8.1` already exist as development pins, and a `v1.0.0` beside them would read as older than them, and
the consumers' pins would seem to go backwards. 2.0.0 is past all of them.
**Alternatives:** 1.0.0 (clashes with the old tags); 1.9.0 (continues a numbering that never meant a release).

## RL005 — Consumers pin candidates on main, releases on release branches

**Date:** 2026-11-24
**Status:** Default (Hongwei may change it).
**Decision:** pp-browser's and pp-ledger-apps' `main` may pin a pp-ledger candidate or release; their release
branches pin releases only. A pp-node release branch that needs a ledger fix takes a pp-ledger patch release from the
branch it pins.
**Rationale:** Release tags now come only from release branches, so a consumer needing a ledger change on main would
otherwise wait for a release. Candidates are immutable tags too, so a pin on one is reproducible; a product release
still ships only on a tested pp-ledger release.
**Alternatives:** Releases only (a release for every pin bump); commit SHAs on main (no tag says what was tested).

## RL006 — A live chain is read forever; rules change at a signed epoch

**Date:** 2026-11-24
**Status:** Accepted (Hongwei, 2026-11-24): the mechanism is built before 2.0.0.
**Decision:** From the network that launches on 2.0.0 on, every release reads and verifies every block that network
made, with the rules it was made under. A consensus change ships in a release that knows both rules and switches on at
an epoch the genesis quorum signs after operators upgraded; a node that does not know the rules a signed activation
names stops before its epoch with "update needed". Re-genesis stays the emergency path. Until then, today's rule
(a format change is a new network) stands.
**Rationale:** A chain's history cannot be re-encoded, and its nodes are run by different people who upgrade at
different times. Without an activation in 2.0.0, the network's first rule change could only be a re-genesis: a new
network id, every signature void, every service and wallet re-pointed. Signed by the genesis quorum like `T_CONFIG`,
so nothing new holds the power to change the rules.
**Alternatives:** Activation at a height compiled into the release (operators who have not upgraded fork off
silently, and a bad height needs another release); re-genesis per rule change (above).

## RL007 — A network launch is not a release

**Date:** 2026-11-24
**Status:** Accepted (Hongwei, 2026-11-24).
**Decision:** Launching a network has its own runbook in this repository: the genesis key ceremony, the beacon's
init, publishing the network id and genesis block hash (the pin consumers trust) and the version table's network row.
Where the nodes run is infrastructure's (Leo's); its hosts, accounts and deploys stay out of this public repository.
**Rationale:** A launch happens once and is about keys and trust, a release happens often and is about bytes; mixing
them would make every release look like a launch, or hide the launch's steps in a release checklist.
