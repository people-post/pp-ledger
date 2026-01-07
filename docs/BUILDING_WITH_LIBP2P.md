# Building with cpp-libp2p

This document describes how to build the pp-ledger project with optional cpp-libp2p support.

## ⚠️ Current Status

**The network library is currently incompatible with the latest cpp-libp2p API.**

The network library code (FetchClient, FetchServer) was written for an older version of cpp-libp2p with different API structure. Building with libp2p is partially configured but will fail during compilation of the network library.

**Working components without libp2p:**
- ✅ Core library (lib)
- ✅ Consensus (Ouroboros implementation)
- ✅ Client
- ✅ Server (Blockchain, Ledger, Wallet)
- ✅ Applications
- ✅ All tests except network tests (134 tests passing)

## Overview

The pp-ledger project can be built with or without cpp-libp2p support:
- **Without libp2p**: Core functionality works (consensus, blockchain, ledger, wallet)
- **With libp2p**: Enables network library for peer-to-peer communication

## Using GitHub Actions Artifacts (Recommended)

### Step 1: Build cpp-libp2p Artifact

The repository includes a GitHub Actions workflow that builds cpp-libp2p and saves it as an artifact.

1. Go to **Actions** → **Build cpp-libp2p** workflow
2. Click **Run workflow** → **Run workflow**
3. Wait for the build to complete (~15-30 minutes)
4. The artifact will be available for 90 days

### Step 2: Build pp-ledger with Artifact

The main build workflow automatically downloads the libp2p artifact if available:

1. Push code or manually trigger **Build pp-ledger** workflow
2. The workflow will:
   - Download the libp2p artifact (if available)
   - Build with libp2p support enabled
   - Run all tests including network tests

## Local Development

### Building Without libp2p (Default)

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
ctest --output-on-failure
```

This builds all components except the network library.

### Building With libp2p

#### Option 1: Using Pre-built Artifact

```bash
# Download and extract the artifact from GitHub Actions
# (assuming you've downloaded libp2p-artifact.tar.gz)
tar -xzf libp2p-artifact.tar.gz

# Build with libp2p
mkdir build && cd build
cmake -DUSE_LIBP2P=ON -DLIBP2P_ROOT=../libp2p-install ..
make -j$(nproc)
ctest --output-on-failure
```

#### Option 2: Building libp2p from Source

```bash
# Clone and build cpp-libp2p
git clone https://github.com/libp2p/cpp-libp2p.git libp2p-src
cd libp2p-src
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../../libp2p-install ..
make -j$(nproc)
make install
cd ../..

# Build pp-ledger with libp2p
mkdir build && cd build
cmake -DUSE_LIBP2P=ON -DLIBP2P_ROOT=../libp2p-install ..
make -j$(nproc)
ctest --output-on-failure
```

## Components Status

| Component | Without libp2p | With libp2p |
|-----------|----------------|-------------|
| lib | ✅ Built | ✅ Built |
| consensus | ✅ Built | ✅ Built |
| network | ❌ Skipped | ✅ Built |
| client | ✅ Built | ✅ Built |
| server | ✅ Built | ✅ Built |
| app | ✅ Built | ✅ Built |
| test_fetch | ❌ Skipped | ✅ Built |

## CMake Options

- `USE_LIBP2P` (default: OFF): Enable cpp-libp2p support
- `LIBP2P_ROOT`: Path to libp2p installation directory

## GitHub Actions Workflows

### build-libp2p.yml

Builds cpp-libp2p and uploads it as an artifact:
- Trigger: Manual (workflow_dispatch) or when the workflow file changes
- Output: `libp2p-build-ubuntu-latest` artifact (retained for 90 days)
- Contents: Compiled libraries and headers in `libp2p-install/`

### build-project.yml

Builds the main pp-ledger project:
- Trigger: Push to main, pull requests, or manual
- Attempts to download libp2p artifact
- Builds with or without libp2p depending on artifact availability
- Runs all applicable tests

## Troubleshooting

### Artifact Not Found

If the build workflow can't find the libp2p artifact:
1. Check if the "Build cpp-libp2p" workflow has run successfully
2. Artifacts expire after 90 days - rebuild if needed
3. The project will build without network library support

### Build Failures

If building with libp2p fails:
1. Ensure `LIBP2P_ROOT` points to the correct installation directory
2. Check that the installation contains `include/libp2p/` and `lib/` directories
3. Try building without libp2p first to verify core functionality

## Dependencies

### System Dependencies (Required)
- C++17 compiler (GCC 13+ or Clang 14+)
- CMake 3.15+
- Boost 1.70+ (system, thread, random, filesystem)
- OpenSSL 3.0+

### Optional (for libp2p support - currently not functional)
- libfmt-dev
- C++20 compiler support
- Additional Boost components (via Hunter in cpp-libp2p)
- Python 3.x

## Future Work

To make the network library work with modern cpp-libp2p, the following changes are needed:

1. **Update network library code** to use current cpp-libp2p APIs
   - Replace `libp2p/protocol/common/asio/asio_scheduler.hpp` with current scheduler API
   - Update Host interface usage
   - Modernize stream handling

2. **Verify libp2p installation** includes all dependencies (qtils, soralog, scale)

3. **Test integration** once API updates are complete

For now, the project builds and runs successfully without libp2p support, providing all core blockchain functionality.
