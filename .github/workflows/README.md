# GitHub Actions Workflows

This directory contains automated workflows for building, testing, and releasing the pp-ledger project. Path-dependent build logic lives in the **`scripts/`** directory at the repo root so CI and local use stay in sync.

## Workflows

### build-project.yml

**Main build and test workflow**

- **Triggers:** Push to `main` / `develop`, pull requests, manual dispatch
- **Runners:** Same OS matrix as [pp-browser](https://github.com/people-post/pp-browser) `build.yml`:
  - `ubuntu-24.04` — full build + test (required)
  - `macos-14` — full build + test (required)
  - `windows-2022` — full build + test (required; MSVC + Ninja + Winsock)
- **Purpose:** Build and test pp-ledger via `scripts/ci-build.sh`

**Steps (per matrix leg):**
1. Checkout code
2. OS-specific deps (apt / Homebrew cmake if missing / Ninja on Windows + MSVC dev cmd)
3. `./scripts/ci-build.sh --build --with-tests`
4. `./scripts/ci-build.sh --run-tests`

### release-docker.yml

**Publish container image (and binary tarball) on version tags**

- **Triggers:** Push of a tag matching `release/v*` (e.g. `release/v1.0.0`), or manual `workflow_dispatch`
- **Runner:** `ubuntu-24.04`
- **Purpose:** Build the multi-stage `Dockerfile` (Ubuntu 24.04) and push to GHCR; on tags, also attach a linux-x64 binary tarball to a GitHub Release
- **Image:** `ghcr.io/<owner>/pp-ledger:<version>` and `:latest` (tags only)

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

### Publishing a release image

```bash
git tag release/v1.0.0
git push origin release/v1.0.0
```

## Dependencies

Fetched at configure time via CMake FetchContent (pp-cpp-common, pp-cpp-crypto). System packages:

| OS | Packages |
|----|----------|
| Linux | `build-essential`, `cmake`, `ninja-build` (CI) |
| macOS | Xcode/clang; `cmake` via Homebrew if absent |
| Windows | MSVC (dev cmd), Ninja (CI); links `ws2_32`, `iphlpapi` |

## Troubleshooting

- **FetchContent failures:** Check tag pins in `cmake/PpCppCommon.cmake` / `cmake/PpCppCrypto.cmake`.
- **Windows socket errors:** Ensure `networkPlatformInit()` runs before socket use (tests call this via `SocketTestUtils.h`).
- **Local repro:** `./scripts/ci-build.sh --test`
