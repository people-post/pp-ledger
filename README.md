# pp-ledger

[![Build pp-ledger](https://github.com/people-post/pp-ledger/actions/workflows/build-project.yml/badge.svg)](https://github.com/people-post/pp-ledger/actions/workflows/build-project.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A blockchain implementation with Ouroboros consensus algorithm, written in C++20.

## Vision

At the heart of PP-Ledger is a commitment to decentralization. This project is not driven by profit but by the desire to create an open, free, and essential tool for the modern decentralized ecosystem.

PP-Ledger aims to be a foundational utility, a barely minimal yet vital tool that ensures the accurate and reliable tracking of event timings. In a world where trust and transparency are becoming increasingly critical, this project aspires to be the simplest possible mechanism for recording when events happen—without unnecessary embellishments or complexity.

By focusing on minimalism and purpose, PP-Ledger provides just what is needed to lay a solid foundation for other systems, processes, and tools that require event tracking in a decentralized environment. It is not the end goal but a stepping stone for decentralized applications and technologies.

## Features

- ✅ **Ouroboros Consensus:** Proof-of-stake consensus with VRF-based slot leader selection
- ✅ **Blockchain & Ledger:** Complete transaction and wallet management
- ✅ **Dual Server Architecture:** Beacon servers (validators) and Miner servers (block producers)
- ✅ **Modular Architecture:** Clean separation of concerns (lib, consensus, ledger, server, client, network)
- ✅ **TCP Networking:** Simple TCP-based peer-to-peer communication
- ✅ **HTTP API Server:** REST-style HTTP server (pp-http) exposing client interfaces for beacon, miner, block, account, and transactions
- ✅ **Comprehensive Testing:** Automated tests with Google Test
- ✅ **CI/CD Pipeline:** GitHub Actions for automated builds and testing

## Architecture

### Server Roles

**Beacon Servers:**
- Network validators and authoritative data sources
- Maintain full blockchain history from genesis (block 0)
- Manage Ouroboros consensus protocol and stakeholder registry
- Do NOT produce blocks (that's the miners' job)
- Implement checkpoint system for data pruning (1GB threshold, 1 year age)
- Limited in number (5-10 globally), run by network founders or elected stakeholders

**Miner Servers:**
- Block producers selected via Ouroboros proof-of-stake
- Maintain transaction pools for pending transactions
- Produce blocks when elected as slot leader
- Stake is registered with and managed by beacon servers
- Selection probability based on registered stake amount
- Sync with beacon servers to stay up-to-date

### Consensus Mechanism

**Ouroboros Proof-of-Stake:**
- Time divided into **slots** (default: 5 seconds)
- Slots grouped into **epochs** (default: 432 slots = ~36 minutes)
- Slot leaders selected using VRF (Verifiable Random Function)
- Selection is stake-weighted and deterministic but unpredictable
- Each slot can have at most one block

## Quick Start

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  libsodium-dev \
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
cd build
ctest --output-on-failure
```

### Quick Test Network

Use the automated script to spin up a local test network (1 beacon + 3 miners):

```bash
# Start with defaults
./test-network.sh

# Start with 5 miners and debug logging
./test-network.sh -n 5 -d

# Clean previous data and start fresh
./test-network.sh -c

# Show all options
./test-network.sh -h
```

The script initializes a beacon on port 8517 and miners on ports 8518+. Stop with Ctrl+C.

**Test the network** (in another terminal):
```bash
./build/app/pp-client -b status
./build/app/pp-client -m add-tx alice bob 100
./build/app/pp-client -b status
```

For detailed server setup, configuration, and troubleshooting, see **[docs/SETUP.md](docs/SETUP.md)**.

## Project Structure

```
pp-ledger/
├── lib/              # Core utilities (logging, serialization, binary packing)
├── consensus/        # Ouroboros PoS consensus (epochs, slots, VRF leader selection)
├── ledger/           # Blockchain storage and management
├── server/           # Beacon and Miner server implementations
├── client/           # TCP client library
├── network/          # Low-level TCP networking
├── app/              # Executables: pp-beacon, pp-miner, pp-client, pp-http
├── node-addon/       # Node.js native addon wrapping the client library
├── scripts/          # Helper scripts
└── docs/             # Documentation
```

## Components

| Component | Description | Status |
|-----------|-------------|--------|
| **lib** | Core utilities, logging, serialization | ✅ Working |
| **consensus** | Ouroboros PoS consensus implementation | ✅ Working |
| **ledger** | Blockchain storage, ledger, wallet management | ✅ Working |
| **server** | Beacon and Miner server implementations | ✅ Working |
| **client** | Client library for server communication | ✅ Working |
| **network** | TCP networking (FetchClient/Server, TcpClient/Server) | ✅ Working |
| **app** | Command-line applications (beacon, miner, client, http API server) | ✅ Working |
| **node-addon** | Node.js native addon for client integration | ✅ Working |

## Documentation

- **[Setup Guide](docs/SETUP.md)** — Full beacon/miner/client setup, configuration reference, and troubleshooting
- **[Server Architecture](server/SERVER.md)** — Server components, APIs, and usage guide
- **[GitHub Actions Setup](docs/GITHUB_ACTIONS_SETUP.md)** — CI/CD configuration

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests: `cd build && ctest --output-on-failure`
5. Submit a pull request

All PRs trigger automated builds and tests via GitHub Actions.

## A Friendly Note to Contributors

To anyone—whether human or AI—who can contribute, improve, and enhance PP-Ledger: your help is deeply appreciated. If you see areas where this project can be made better, through clearer code, greater efficiency, or by simply bringing in your unique perspectives, please don't hesitate to join in. Collaboration and diverse contributions strengthen this project's foundation for decentralization, and we welcome your input with open arms.

Let's build something meaningful together.

## License

See [LICENSE](LICENSE) file for details.
