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

### Build

```bash
mkdir build && cd build
cmake ..
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
| **network** | P2P networking (FetchClient/Server) | ⚠️ Requires libp2p |
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

## Current Limitations

### Network Library

⚠️ The network library (FetchClient/FetchServer) uses older cpp-libp2p APIs and is currently disabled.

**Impact:** Core blockchain functionality works perfectly, but P2P networking is not available.

**Workaround:** The project builds and runs without the network library.

See [docs/BUILDING_WITH_LIBP2P.md](docs/BUILDING_WITH_LIBP2P.md) for technical details and future plans.

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

- **Language:** C++17
- **Build System:** CMake 3.15+
- **Testing:** Google Test
- **Dependencies:** Boost 1.70+, OpenSSL 3.0+, fmt
- **CI/CD:** GitHub Actions
