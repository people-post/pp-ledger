# pp-ledger

A blockchain implementation with Ouroboros consensus algorithm, written in C++17.

## Features

- ✅ **Ouroboros Consensus:** Proof-of-stake consensus with VRF-based slot leader selection
- ✅ **Blockchain & Ledger:** Complete transaction and wallet management
- ✅ **Modular Architecture:** Clean separation of concerns (lib, consensus, server, client)
- ✅ **Comprehensive Testing:** 134 automated tests with Google Test
- ✅ **CI/CD Pipeline:** GitHub Actions for automated builds and testing

## Quick Start

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  libssl-dev \
  libboost-all-dev \
  libfmt-dev \
  python3
```

### Build cpp-libp2p

First, build and install cpp-libp2p:

```bash
git clone https://github.com/libp2p/cpp-libp2p.git
cd cpp-libp2p
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/path/to/libp2p-install ..
make -j$(nproc)
cmake --install .
```

Or download the pre-built artifact from GitHub Actions (see [docs/BUILDING_WITH_LIBP2P.md](docs/BUILDING_WITH_LIBP2P.md)).

### Build

```bash
mkdir build && cd build
cmake -DLIBP2P_ROOT=/path/to/libp2p-install ..
make -j$(nproc)
```

### Run Tests

```bash
ctest --output-on-failure
```

Expected result: **134/134 tests passing**

## Project Structure

```
pp-ledger/
├── lib/              # Core library (Logger, Module, ResultOrError)
├── consensus/        # Ouroboros consensus implementation
├── server/           # Blockchain, Ledger, Wallet
├── client/           # Client library
├── network/          # Network library (requires cpp-libp2p)
├── app/              # Client and server applications
├── test/             # Comprehensive test suite
└── docs/             # Documentation
```

## Components

| Component | Description | Status |
|-----------|-------------|--------|
| **lib** | Core utilities and logging | ✅ Working |
| **consensus** | Ouroboros PoS consensus | ✅ Working |
| **server** | Blockchain, Ledger, Wallet | ✅ Working |
| **client** | Client library | ✅ Working |
| **network** | P2P networking (FetchClient/Server) | ✅ Working (C++20) |
| **app** | Command-line applications | ✅ Working |

## Documentation

- **[GitHub Actions Setup](docs/GITHUB_ACTIONS_SETUP.md)** - CI/CD configuration and artifact management
- **[Building with libp2p](docs/BUILDING_WITH_LIBP2P.md)** - Optional P2P networking support
- **[Workflow README](.github/workflows/README.md)** - GitHub Actions workflow details

## Development

### Running Applications

```bash
# Start server
./build/app/pp-ledger-server

# Run client
./build/app/pp-ledger-client
```

### Running Specific Tests

```bash
cd build
./test/test_ouroboros_consensus
./test/test_blockchain
./test/test_ledger
```

## GitHub Actions

### Automated Builds

Every push to `main` triggers automated builds and tests.

### libp2p Artifact Build

To build cpp-libp2p as a GitHub artifact:

1. Go to **Actions** → **Build cpp-libp2p**
2. Click **Run workflow**
3. Artifact will be available for 90 days

See [docs/GITHUB_ACTIONS_SETUP.md](docs/GITHUB_ACTIONS_SETUP.md) for details.

## Dependencies

### Required

- **C++17 compiler** (GCC 13+, Clang 14+) for core components
- **C++20 compiler** for network library
- **CMake 3.15+**
- **Boost 1.70+** (system, thread, random, filesystem)
- **OpenSSL 3.0+**
- **libfmt** (for cpp-libp2p)
- **cpp-libp2p** (built from source or GitHub artifact)

### Building cpp-libp2p

See [docs/BUILDING_WITH_LIBP2P.md](docs/BUILDING_WITH_LIBP2P.md) for detailed instructions on:
- Building cpp-libp2p from source
- Downloading pre-built artifacts from GitHub Actions
- Setting up Hunter dependencies (qtils, soralog, scale)

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests: `ctest --output-on-failure`
5. Submit a pull request

All PRs trigger automated builds and tests via GitHub Actions.

## License

See [LICENSE](LICENSE) file for details.

## Technical Details

- **Language:** C++17 (core), C++20 (network)
- **Build System:** CMake 3.15+
- **Testing:** Google Test
- **Dependencies:** Boost 1.70+, OpenSSL 3.0+, fmt, cpp-libp2p
- **CI/CD:** GitHub Actions
