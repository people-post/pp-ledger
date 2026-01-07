# GitHub Actions Workflows

This directory contains automated workflows for building and testing the pp-ledger project.

## Workflows

### 1. build-project.yml

**Main build and test workflow**

- **Triggers:** Push to main, pull requests, manual dispatch
- **Purpose:** Build and test the pp-ledger project
- **Features:**
  - Installs system dependencies (Boost, OpenSSL, fmt)
  - Attempts to download pre-built libp2p artifact (optional)
  - Builds project with or without libp2p based on artifact availability
  - Runs all applicable tests (134 tests)
  
**Steps:**
1. Checkout code
2. Install system dependencies
3. Try to download libp2p artifact from previous runs
4. Configure CMake (with `-DUSE_LIBP2P=ON` if artifact available)
5. Build with `make`
6. Run tests with `ctest`

### 2. build-libp2p.yml

**Build cpp-libp2p and create artifact**

- **Triggers:** Manual dispatch, changes to workflow file
- **Purpose:** Build cpp-libp2p from source and save as artifact
- **Artifact retention:** 90 days
- **Features:**
  - Clones cpp-libp2p from GitHub
  - Builds with CMake in Release mode
  - Installs to a prefix directory
  - Copies Hunter dependencies (qtils, scale, soralog)
  - Packages as tar.gz artifact

**Steps:**
1. Checkout code
2. Install build dependencies
3. Clone cpp-libp2p repository
4. Build cpp-libp2p (15-30 minutes)
5. Install to libp2p-install directory
6. Copy Hunter dependency headers
7. Package as artifact
8. Upload artifact (retained for 90 days)

## Usage

### Running the Workflows

#### Build Main Project

The main build runs automatically on pushes and PRs. To manually trigger:

1. Go to **Actions** tab
2. Select **Build pp-ledger**
3. Click **Run workflow**
4. Select branch and click **Run workflow**

#### Build libp2p Artifact

To create a fresh libp2p artifact:

1. Go to **Actions** tab
2. Select **Build cpp-libp2p**
3. Click **Run workflow**
4. Select branch and click **Run workflow**
5. Wait ~15-30 minutes for completion
6. Artifact will be available for 90 days

### Downloading Artifacts Locally

To use the libp2p artifact locally:

```bash
# Download artifact from GitHub Actions UI or API
# Extract it
tar -xzf libp2p-artifact.tar.gz

# Build with libp2p
mkdir build && cd build
cmake -DUSE_LIBP2P=ON -DLIBP2P_ROOT=../libp2p-install ..
make -j$(nproc)
```

## Dependencies

Both workflows install the following system packages:
- build-essential (GCC, make, etc.)
- cmake
- git
- libssl-dev
- libboost-all-dev
- libfmt-dev
- python3

## Current Limitations

⚠️ **Network Library Compatibility Issue**

The network library code uses older cpp-libp2p APIs that are not compatible with the current version. Even with the libp2p artifact, the network library will fail to compile.

**What works:**
- Core library
- Consensus module
- Client/Server
- All non-network tests (134 passing)

**What doesn't work:**
- Network library (FetchClient/FetchServer)
- Network tests (test_fetch)

See [docs/BUILDING_WITH_LIBP2P.md](../docs/BUILDING_WITH_LIBP2P.md) for details and future work needed.

## Workflow Configuration

### Environment Variables

build-project.yml sets:
- `LIBP2P_AVAILABLE`: Set to "1" if artifact downloaded successfully, "0" otherwise

### Artifacts

**libp2p-build-ubuntu-latest:**
- Name: `libp2p-build-ubuntu-latest`
- File: `libp2p-artifact.tar.gz`
- Contents: 
  - `libp2p-install/include/` - Headers (libp2p + dependencies)
  - `libp2p-install/lib/` - Static libraries
- Retention: 90 days
- Size: ~50-100 MB

## Troubleshooting

### Artifact Not Found Warning

If you see "WARNING: Artifact not found" in build-project.yml:
- This is normal if build-libp2p.yml hasn't run yet
- The project will build without libp2p (which is fine)
- Run build-libp2p.yml manually to create the artifact

### Build Failures

If builds fail:
1. Check the workflow logs for specific errors
2. Ensure dependencies are properly installed
3. Try running locally with the same commands
4. For network library issues, see docs/BUILDING_WITH_LIBP2P.md

### Artifact Expiration

Artifacts expire after 90 days. To refresh:
1. Manually run build-libp2p.yml workflow
2. Wait for completion
3. Future builds will use the new artifact

## Future Improvements

Planned enhancements:
- [ ] Update network library to work with current cpp-libp2p
- [ ] Add caching for faster builds
- [ ] Create release artifacts
- [ ] Add deployment workflows
- [ ] Run tests in parallel
- [ ] Add code coverage reporting
