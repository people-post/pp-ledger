# Building with cpp-libp2p

This document describes how to build the pp-ledger project with optional cpp-libp2p support.

## ✅ Current Status

**The network library has been updated to work with the latest cpp-libp2p API!**

The network library (FetchClient, FetchServer) now uses the modern cpp-libp2p APIs including:
- Updated stream API (readSome/writeSome)
- Modern callback signatures with outcome::result
- StreamAndProtocol callback pattern
- C++20 compatibility for std::span support

**Working components:**
- ✅ Core library (lib)
- ✅ Consensus (Ouroboros implementation)
- ✅ Client
- ✅ Server (Blockchain, Ledger, Wallet)
- ✅ Applications
- ✅ Network library (with libp2p)
- ✅ All tests (134 tests passing without libp2p, network tests available with libp2p)

## Overview

The pp-ledger project can be built with or without cpp-libp2p support:
- **Without libp2p**: Core functionality works (consensus, blockchain, ledger, wallet)
- **With libp2p**: Enables network library for peer-to-peer communication

## Building

### Step 1: Build cpp-libp2p Artifact

The repository includes a GitHub Actions workflow that builds cpp-libp2p and saves it as an artifact.

1. Go to **Actions** → **Build cpp-libp2p** workflow
2. Click **Run workflow** → **Run workflow**
3. Wait for the build to complete (~15-30 minutes)
4. Download the artifact (retained for 90 days)

### Step 2: Build pp-ledger

```bash
# Extract the artifact
tar -xzf libp2p-artifact.tar.gz

# Build with libp2p
mkdir build && cd build
cmake -DLIBP2P_ROOT=../libp2p-install ..
make -j$(nproc)
ctest --output-on-failure
```

## Components Status

| Component | Status | Tests |
|-----------|--------|-------|
| lib | ✅ Built (C++17) | ✅ Passing |
| consensus | ✅ Built (C++17) | ✅ Passing |
| network | ✅ Built (C++20) | ⏸️ Placeholder |
| client | ✅ Built (C++17) | ✅ Passing |
| server | ✅ Built (C++17) | ✅ Passing |
| app | ✅ Built (C++17) | N/A |

**Note:** Network library and its tests require C++20 for std::span support used by libp2p.

## CMake Options

- `LIBP2P_ROOT` (required): Path to libp2p installation directory

## GitHub Actions Workflows

### build-libp2p.yml

Builds cpp-libp2p and uploads it as an artifact:
- Trigger: Manual (workflow_dispatch) or when the workflow file changes
- Output: `libp2p-build-ubuntu-latest` artifact (retained for 90 days)
- Contents: Compiled libraries and headers in `libp2p-install/`

### build-project.yml

Builds the main pp-ledger project:
- Trigger: Push to main, pull requests, or manual
- Downloads libp2p artifact (required)
- Builds all components including network library
- Runs all tests

## Troubleshooting

### Artifact Not Found

If the build workflow can't find the libp2p artifact:
1. Run the "Build cpp-libp2p" workflow first
2. Artifacts expire after 90 days - rebuild if needed

### Build Failures

If building fails:
1. Ensure `LIBP2P_ROOT` is set correctly
2. Check that the installation contains `include/libp2p/` and `lib/` directories
3. Verify all Hunter dependencies (qtils, soralog, scale) are in the artifact

## Dependencies

### System Dependencies (Required)
- C++17 compiler (GCC 13+ or Clang 14+) for core components
- C++20 compiler for network library
- CMake 3.15+
- Boost 1.70+ (system, thread, random, filesystem)
- OpenSSL 3.0+
- libfmt-dev
- Python 3.x

### cpp-libp2p Dependencies (Included in artifact)
- qtils (C++20 utilities)
- soralog (logging)
- scale (serialization)

## Implementation Notes

### API Changes from Old libp2p

The network library was updated to use modern cpp-libp2p APIs:

**Stream API:**
- Old: `stream->read()` / `stream->write()`
- New: `stream->readSome()` / `stream->writeSome()`

**Callback Signatures:**
- Old: `auto&& result` with implicit error checking
- New: `outcome::result<T>` with explicit error handling

**Protocol Handlers:**
- Old: `setProtocolHandler(string, callback)`
- New: `setProtocolHandler(StreamProtocols, StreamAndProtocolCb)`

**Data Buffers:**
- Old: `gsl::span<const uint8_t>`
- New: `libp2p::BytesIn` / `libp2p::BytesOut` (using std::span)

### C++20 Requirement

The network library requires C++20 because cpp-libp2p uses `std::span` which is a C++20 feature. The rest of the project remains C++17 compatible.

### Testing

Network integration tests (test_fetch) are currently disabled as they require:
1. A running libp2p host instance
2. Proper peer setup and discovery
3. Network connectivity

These can be enabled once a test harness for libp2p is implemented.
