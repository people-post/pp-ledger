# GitHub Actions Workflows

This directory contains automated workflows for building, testing, and releasing the pp-ledger project. Path-dependent build and packaging logic lives in the **`scripts/`** directory at the repo root so CI and local use stay in sync.

## Workflows

### build-project.yml

**Main build and test workflow**

- **Triggers:** Push to main, pull requests, manual dispatch
- **Purpose:** Build and test the pp-ledger project
- **Features:**
  - Installs system dependencies (build-essential, cmake, libsodium, nlohmann-json)
  - Builds via `scripts/ci-build.sh --test` (configure, build, run ctest)

**Steps:**
1. Checkout code
2. Install system dependencies
3. Build and test (single step: `./scripts/ci-build.sh --test`)

### release-node-addon.yml

**Release the Node addon on version tags**

- **Triggers:** Push of a tag matching `release/v*` (e.g. `release/v1.0.0`)
- **Purpose:** Build the Node.js N-API addon, package it with the JS wrapper and package.json, and create a GitHub Release with the tarball attached
- **Features:**
  - Installs system deps and Node.js, runs `npm ci` in `node-addon/`
  - Builds with `scripts/ci-build.sh --node-addon`
  - Packages with `scripts/package-node-addon.sh <version> linux-x64`
  - Creates a GitHub Release for the tag and uploads the tarball (e.g. `pp-ledger-node-addon-1.0.0-linux-x64.tar.gz`)

**Steps:**
1. Checkout, install system dependencies, setup Node.js
2. Install node-addon npm dependencies
3. Build with node addon
4. Package addon (staging + tarball)
5. Create GitHub Release and upload the tarball

## Scripts (repo root)

Path-dependent work is centralized in `scripts/` so workflows avoid hardcoded paths and commands:

- **`scripts/ci-build.sh`** — Resolves repo root, configures CMake (`build/`), builds. Options: `--node-addon` (build the Node addon), `--test` (run ctest after build).
- **`scripts/package-node-addon.sh`** — Stages the built addon binary plus `index.js` and `package.json` into a directory, creates a tarball. Usage: `./scripts/package-node-addon.sh <version> [platform]` or set `VERSION` and optionally `PLATFORM` in the environment. Output path is printed for CI to upload.

## Usage

### Running the main build

The main build runs automatically on pushes and PRs. To manually trigger:

1. Go to **Actions** tab
2. Select **Build pp-ledger**
3. Click **Run workflow**
4. Select branch and click **Run workflow**

### Creating a release

Push a tag that matches `release/v*`:

```bash
git tag release/v1.0.0
git push origin release/v1.0.0
```

The **Release Node addon** workflow will run, build the addon, package it, and create a GitHub Release with the tarball attached.

## Dependencies

Workflows install the following system packages:
- build-essential (GCC, make, etc.)
- cmake
- libsodium-dev
- nlohmann-json3-dev

The release workflow also uses Node.js (e.g. 20 LTS) and runs `npm ci` in `node-addon/`.

## Troubleshooting

### Build failures

If builds fail:
1. Check workflow logs for specific errors
2. Ensure all dependencies are correctly specified
3. Verify the CMakeLists.txt configuration is correct
4. Run the same steps locally via `./scripts/ci-build.sh [--node-addon] [--test]`

### Release failures

If the release workflow fails:
1. Ensure the tag matches `release/v*` (e.g. `release/v1.0.0`)
2. Run `./scripts/ci-build.sh --node-addon` then `./scripts/package-node-addon.sh 1.0.0 linux-x64` locally to verify packaging

## Future improvements

Planned enhancements:
- [ ] Add caching for faster builds
- [ ] Multi-platform release (e.g. macOS, Windows) via matrix
- [ ] Run addon smoke test in release workflow
- [ ] Run tests in parallel
- [ ] Add code coverage reporting
