# Current state — release

**As of:** 2026-11-24

| Phase | Status | Notes |
|-------|--------|-------|
| R1 mechanics | Built, not yet run | `release.yml` (replaces `release-docker.yml`), backport bot, release-branch PR check, release issue, [RELEASE.md](../../docs/ops/RELEASE.md). The candidate's image smoke was run locally on a prebuilt image (L-SMOKE-IMAGE passed). Left: the repository settings, pp-browser's RC023 |
| R2 compatibility tests | Not started | Build identity first: nodes do not report their version today |
| R3 rule activation | Not started | Needs its own design; before 2.0.0 |
| R4 network launch | Not started | Hosting waits for Leo |
| R5 2.0.0 | Not started | After R2's first two items and R3 |

Decisions RL001–RL004, RL006 and RL007 taken by Hongwei on 2026-11-24; RL005 (what consumers pin) is a default.
