# GitHub Actions Workflows

This directory contains automated workflows for building and testing the pp-ledger project. Path-dependent build logic lives in the **`scripts/`** directory at the repo root so CI and local use stay in sync.

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

## Scripts (repo root)

Path-dependent work is centralized in `scripts/` so workflows avoid hardcoded paths and commands:

- **`scripts/ci-build.sh`** — Resolves repo root, configures CMake (`build/`), builds. Options: `--test` (run ctest after build).

## Usage

### Running the main build

The main build runs automatically on pushes and PRs. To manually trigger:

1. Go to **Actions** tab
2. Select **Build pp-ledger**
3. Click **Run workflow**
4. Select branch and click **Run workflow**

## Dependencies

Workflows install the following system packages:
- build-essential (GCC, make, etc.)
- cmake
- libsodium-dev
- nlohmann-json3-dev

## Troubleshooting

### Build failures

If builds fail:
1. Check workflow logs for specific errors
2. Ensure all dependencies are correctly specified
3. Verify the CMakeLists.txt configuration is correct
4. Run the same steps locally via `./scripts/ci-build.sh [--test]`

## Future improvements

Planned enhancements:
- [ ] Add caching for faster builds
- [ ] Run tests in parallel
- [ ] Add code coverage reporting
