---
name: Release
about: One per release, opened when its release branch is cut; the checklist and the go / no-go record
title: "Release pp-ledger X.Y"
labels: release
---

<!-- Opened by .github/workflows/release-issue.yml when release/X.Y is cut; by hand, fill the <placeholders>.
Steps: docs/ops/RELEASE.md. Link what CI and the workflows check (runs, the candidate's prerelease) rather than
re-checking it; strike what this release does not change. Operator hosts and deploys are infrastructure's (Leo):
link their record, do not copy it here (this repository is public). -->

- **Branch:** `<branch>` · **Tags:** `vX.Y.Z`, candidates `vX.Y.Z-rc.N` · **Release owner:** @<owner>
- **Go / no-go:** pending <!-- go or no-go, the date, who decided, why -->
- **Backports to this branch:** label the merged main PR `backport <branch>` (docs/ops/RELEASE.md § Release branches and backports)

### Before the cut
- [ ] Format and protocol changes since the last release listed (block, genesis meta, account, RPC, certificate, node files), each with how a node on the last release meets it
- [ ] Consensus changes: none, or each with its activation (RL006) and the epoch it is proposed for
- [ ] The version table in docs/ops/RELEASE.md updated for X.Y
- [ ] pp-cpp-* pins are release tags

### Release candidate
- [ ] `vX.Y.Z-rc.N` tagged on `<branch>`; the candidate's prerelease exists (image built once, image smoke passed)
- [ ] Build CI green on `<branch>` (Linux, macOS, Windows; every test)
- [ ] Frozen-chain tests green: chains from every release in the version table load and verify
- [ ] Smokes on the candidate's binaries: `smoke`, `rotation`, `takeover`, `move`, `regenesis`, `upstream`, `topology`
- [ ] Mixed versions: this candidate's relay and miners against the last release's beacon, and the reverse
- [ ] pp-node (pp-browser) and pp-ledger-apps built against the candidate tag, their tests green

### Go / no-go
- [ ] No open blocker; known issues listed
- [ ] Rollback path written: the last release's image, and whether this one's node files can be read by it
- [ ] Operators' upgrade order written (nodes that read a new form before those that write it; with an activation, every node before its epoch)

### Rollout
- [ ] `vX.Y.Z` tagged on the candidate's commit; the release lists the image digest and SHA-256s
- [ ] Our nodes upgraded (infrastructure's record linked); other operators told, with the upgrade order and any activation epoch
- [ ] pp-browser and pp-ledger-apps pins moved to `vX.Y.Z` (and their release branches, where the fix is needed)

### After
- [ ] An activation proposed in this release: signed by the genesis quorum once the operators upgraded, and recorded here
- [ ] Anything went wrong: an issue, its finding turned into a test
