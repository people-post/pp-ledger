# pp-ledger

A blockchain implementation with Ouroboros consensus algorithm, written in C++20.

## Features

- ✅ **Ouroboros Consensus:** Proof-of-stake consensus with VRF-based slot leader selection
- ✅ **Blockchain & Ledger:** Complete transaction and wallet management
- ✅ **Modular Architecture:** Clean separation of concerns (lib, consensus, server, client)
- ✅ **TCP Networking:** Simple TCP-based peer-to-peer communication
- ✅ **Comprehensive Testing:** Automated tests with Google Test
- ✅ **CI/CD Pipeline:** GitHub Actions for automated builds and testing

## Quick Start

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  libssl-dev \
  nlohmann-json3-dev
```

### Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Run Tests

```bash
ctest --output-on-failure
```

## Project Structure

```
pp-ledger/
├── lib/              # Core library (Logger, Module, ResultOrError)
├── consensus/        # Ouroboros consensus implementation
├── ledger/           # Blockchain, Ledger, Wallet
├── server/           # Server implementation
├── client/           # Client library
├── network/          # Network library (FetchClient/Server, TcpClient/Server)
├── app/              # Client and server applications
└── docs/             # Documentation
```

## Components

| Component | Description | Status |
|-----------|-------------|--------|
| **lib** | Core utilities and logging | ✅ Working |
| **consensus** | Ouroboros PoS consensus | ✅ Working |
| **ledger** | Blockchain, Ledger, Wallet | ✅ Working |
| **server** | Server with P2P support | ✅ Working |
| **client** | Client library | ✅ Working |
| **network** | TCP networking (FetchClient/Server) | ✅ Working |
| **app** | Command-line applications | ✅ Working |

## Documentation

- **[GitHub Actions Setup](docs/GITHUB_ACTIONS_SETUP.md)** - CI/CD configuration
- **[Workflow README](.github/workflows/README.md)** - GitHub Actions workflow details

## Development

### Running Applications

```bash
# Start server
./build/app/pp-server

# Run client
./build/app/pp-client
```

### Running Specific Tests

```bash
cd build
./consensus/test/test_ouroboros_consensus
./server/test/test_blockchain
./ledger/test/test_ledger
```

## GitHub Actions

### Automated Builds

Every push to `main` triggers automated builds and tests.

See [docs/GITHUB_ACTIONS_SETUP.md](docs/GITHUB_ACTIONS_SETUP.md) for details.

## Dependencies

### Required

- **C++20 compiler** (GCC 10+, Clang 12+)
- **CMake 3.15+**
- **OpenSSL** (for cryptographic hashing)
- **nlohmann/json** (for JSON serialization)

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

- **Language:** C++20
- **Build System:** CMake 3.15+
- **Testing:** Google Test
- **Dependencies:** OpenSSL, nlohmann/json
- **CI/CD:** GitHub Actions
