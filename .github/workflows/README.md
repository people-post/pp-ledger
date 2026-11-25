# GitHub Actions Workflows

This directory contains automated workflows for building, testing, and releasing the pp-ledger project. Path-dependent build logic lives in the **`scripts/`** directory at the repo root so CI and local use stay in sync.

## Workflows

### build-project.yml

**Main build and test workflow**

- **Triggers:** Push to `main` and `release/**`, pull requests to them, manual dispatch (the backport bot dispatches it on its branches)
- **Runners:** Same OS matrix as [pp-browser](https://github.com/people-post/pp-browser) `build.yml`:
  - `ubuntu-24.04` — full build + test (required)
  - `macos-15` — full build + test (required)
  - `windows-2022` — full build + test (required; MSVC + Ninja + Winsock)
- **Purpose:** Build and test pp-ledger via `scripts/ci-build.sh`

**Steps (per matrix leg):**
1. Checkout code
2. OS-specific deps (apt / Homebrew cmake if missing / Ninja on Windows + MSVC dev cmd)
3. `./scripts/ci-build.sh --build --with-tests`
4. `./scripts/ci-build.sh --run-tests`

### release.yml

**Release candidates, releases and dev images** ([docs/ops/RELEASE.md](../../docs/ops/RELEASE.md))

- **Triggers:** Push of a tag `vX.Y.Z-rc.<n>` or `vX.Y.Z` (on `release/X.Y`), or manual `workflow_dispatch` with a ref on main
- **Runner:** `ubuntu-24.04`
- **Candidate:** builds the `Dockerfile` once, pushes `ghcr.io/<owner>/pp-ledger:X.Y.Z-rc.<n>`, runs the image smoke on it, attests provenance, publishes a prerelease with the linux-x64 tarball
- **Release:** no build; re-tags the candidate's image `X.Y.Z` (and `latest` for the newest release) and releases its tarball
- **Dev image:** `0.0.0-dev.<sha>` from main, untagged

### backport.yml, release-branch-pr.yml, release-issue.yml

The release branches' bots, as pp-browser's: a merged main PR labelled `backport release/X.Y` gets a cherry-pick PR to
that branch; a PR to a release branch must be a backport or carry `release-branch-only` with its reason; cutting
`release/X.Y` opens its release issue and label. [RELEASE.md § Release branches and backports](../../docs/ops/RELEASE.md#release-branches-and-backports).

See **[deploy/README.md](../../deploy/README.md)** for running the published image with Compose.

## Scripts (repo root)

- **`scripts/ci-build.sh`** — Configures CMake (`build/`), builds, and/or runs ctest.
  Honors `RUNNER_OS` (set in CI) for Windows Ninja + `cmake --build --config Release`
  and `ctest -C Release`. Flags:
  - `--build` — configure + build
  - `--with-tests` — enable `PP_LEDGER_BUILD_TESTS=ON`
  - `--run-tests` — ctest only
  - `--test` — shorthand for build + with-tests + run-tests (local convenience)

## Usage

### Running the main build locally

```bash
./scripts/ci-build.sh --test
# or separately:
./scripts/ci-build.sh --build --with-tests
./scripts/ci-build.sh --run-tests
```

### Manual workflow dispatch

1. Go to **Actions** → **Build pp-ledger**
2. **Run workflow** → pick branch

### Publishing a release

Cut `release/X.Y` from main, tag a candidate on it, then the release on the candidate's commit:
[docs/ops/RELEASE.md § Maintainer flow](../../docs/ops/RELEASE.md#maintainer-flow).

## Dependencies

Fetched at configure time via CMake FetchContent (pp-cpp-common, pp-cpp-crypto). System packages:

| OS | Packages |
|----|----------|
| Linux | `build-essential`, `cmake`, `ninja-build` (CI) |
| macOS | Xcode/clang; `cmake` via Homebrew if absent |
| Windows | MSVC (dev cmd), Ninja (CI); links `ws2_32`, `iphlpapi` |

## Troubleshooting

- **FetchContent failures:** Check tag pins in `cmake/PpCppCommon.cmake` / `cmake/PpCppCrypto.cmake`.
- **Local repro:** `./scripts/ci-build.sh --test`
