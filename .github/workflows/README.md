# GitHub Actions Workflows

This directory contains automated workflows for building and testing the pp-ledger project.

**Note:** All builds require cpp-libp2p as a dependency.

## Workflows

### 1. build-project.yml

**Main build and test workflow**

- **Triggers:** Push to main, pull requests, manual dispatch
- **Purpose:** Build and test the pp-ledger project
- **Features:**
  - Installs system dependencies (Boost, OpenSSL, fmt)
  - Downloads pre-built libp2p artifact (required)
  - Builds all components including network library
  - Runs all tests (134 tests)
  
**Steps:**
1. Checkout code
2. Install system dependencies
3. Download libp2p artifact (required)
4. Extract artifact
5. Configure CMake with LIBP2P_ROOT
6. Build with `make`
7. Run tests with `ctest`

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
cmake -DLIBP2P_ROOT=../libp2p-install ..
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

### Network Integration Tests

Network integration tests (test_fetch) are currently placeholder tests because they require:
- A running libp2p host instance
- Proper peer setup and discovery
- Network connectivity between test peers

The network library itself builds and compiles successfully with libp2p support.

See [docs/BUILDING_WITH_LIBP2P.md](../docs/BUILDING_WITH_LIBP2P.md) for technical details.

## Workflow Configuration

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

### Artifact Not Found

If you see "Artifact not found" in build-project.yml:
- Run build-libp2p.yml workflow first to create the artifact
- The artifact is required for all builds
- Artifacts expire after 90 days

### Build Failures

If builds fail:
1. Ensure the libp2p artifact was successfully created
2. Check workflow logs for specific errors
3. Verify LIBP2P_ROOT is set correctly in the workflow
4. For local builds, ensure libp2p-install directory structure is correct

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
