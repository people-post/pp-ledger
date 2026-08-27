# GitHub Actions Workflows

This directory contains automated workflows for building, testing, and releasing the pp-ledger project. Path-dependent build logic lives in the **`scripts/`** directory at the repo root so CI and local use stay in sync.

CI runners and the Docker image both use **Ubuntu 24.04**.

## Workflows

### build-project.yml

**Main build and test workflow**

- **Triggers:** Push to main, pull requests, manual dispatch
- **Runner:** `ubuntu-24.04`
- **Purpose:** Build and test the pp-ledger project
- **Features:**
  - Installs system dependencies (build-essential, cmake)
  - Builds via `scripts/ci-build.sh --test` (configure, build, run ctest)

**Steps:**
1. Checkout code
2. Install system dependencies
3. Build and test (single step: `./scripts/ci-build.sh --test`)

### release-docker.yml

**Publish container image (and binary tarball) on version tags**

- **Triggers:** Push of a tag matching `release/v*` (e.g. `release/v1.0.0`), or manual `workflow_dispatch`
- **Runner:** `ubuntu-24.04`
- **Purpose:** Build the multi-stage `Dockerfile` (Ubuntu 24.04) and push to GHCR; on tags, also attach a linux-x64 binary tarball to a GitHub Release
- **Image:** `ghcr.io/<owner>/pp-ledger:<version>` and `:latest` (tags only)
- **Features:**
  - Release build includes `pp-http` (`BUILD_HTTP=ON`)
  - Buildx + GHA cache
  - Binaries extracted from the published image for the release tarball

**Steps:**
1. Checkout, derive version from tag (`release/v1.0.0` → `1.0.0`)
2. Log in to `ghcr.io`, build/push image
3. On tags: package `pp-ledger-linux-x64-<version>.tar.gz` and create GitHub Release

## Scripts (repo root)

- **`scripts/ci-build.sh`** — Resolves repo root, configures CMake (`build/`), builds. Options: `--test` (run ctest after build).

## Usage

### Running the main build

The main build runs automatically on pushes and PRs. To manually trigger:

1. Go to **Actions** tab
2. Select **Build pp-ledger**
3. Click **Run workflow**
4. Select branch and click **Run workflow**

### Publishing a release image

```bash
git tag release/v1.0.0
git push origin release/v1.0.0
```

See **[deploy/README.md](../../deploy/README.md)** for running the published image with Compose.

## Dependencies

Workflows install the following system packages:
- build-essential (GCC, make, etc.)
- cmake

JSON is vendored under `src/lib/`. Crypto is linked statically from pp-cpp-crypto, so Docker runtime images do not need `libsodium23` or `nlohmann-json3-dev`.

## Troubleshooting

### Build failures

If builds fail:
1. Check workflow logs for specific errors
2. Ensure all dependencies are correctly specified
3. Verify the CMakeLists.txt configuration is correct
4. Run the same steps locally via `./scripts/ci-build.sh [--test]`

### Release / image failures

1. Ensure the tag matches `release/v*` (e.g. `release/v1.0.0`)
2. Confirm `packages: write` permission is available to the workflow (default `GITHUB_TOKEN` is enough for GHCR in this repo)
3. Build locally: `docker build -t pp-ledger:local .`

## Future improvements

Planned enhancements:
- [ ] Add caching for faster builds
- [ ] Run tests in parallel
- [ ] Add code coverage reporting
- [ ] Multi-arch images (e.g. arm64) via buildx matrix
