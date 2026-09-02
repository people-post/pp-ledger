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
- ✅ **Relay Server:** Trusted gateway — same ledger RPC as beacon; miners use opaque upstream endpoints
- ✅ **Modular Architecture:** Clean separation of concerns (lib, consensus, ledger, server, client, network)
- ✅ **AMP Networking:** UDP/ADP fleet transport (`/pp-ledger/rpc/1.0.0`) via pp-cpp-amp
- ✅ **HTTP API Server:** REST-style HTTP server (pp-http) exposing client interfaces for beacon, miner, block, account, and transactions
- ✅ **Comprehensive Testing:** Automated tests with Google Test
- ✅ **CI/CD Pipeline:** GitHub Actions for automated builds, tests, and Docker image releases

## Architecture

### Server Roles

**Beacon Servers:**
- Network validators and authoritative data sources
- Maintain full blockchain history from genesis (block 0)
- Manage Ouroboros consensus protocol and stakeholder registry
- Do NOT produce blocks (that's the miners' job)
- Implement checkpoint system for data pruning (1GB threshold, 1 year age)
- Limited in number (5-10 globally), run by network founders or elected stakeholders

**Relay Servers:**
- Trusted gateways between the terminal beacon and participants
- Expose the **same ledger RPC** as the beacon — miners do not distinguish relay from beacon
- `beacon` in relay config is an **upstream** endpoint (terminal beacon or another relay)
- Sync blocks from upstream; forward mutating RPCs (e.g. `BLOCK_ADD`) write-through
- Do NOT produce blocks
- Run `pp-relay` binary

**Miner Servers:**
- Block producers selected via Ouroboros proof-of-stake
- Maintain transaction pools for pending transactions
- Produce blocks when elected as slot leader
- Stake is registered with and managed by beacon servers
- Selection probability based on registered stake amount
- `beacons[]` lists opaque upstream endpoints (relay and/or beacon); sync and broadcast use this set

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
  cmake
```

Shared C++ foundation comes from **pp-cpp-common** (`pp_common`: Logger, Module, Roe, Serialize, Value/Meta/JSON, …). Crypto comes from **pp-cpp-crypto** (`pp_crypto`: libsodium + ML-DSA-65 / ML-KEM-768). Both are fetched via CMake FetchContent (tags `pp-cpp-common` `v0.2.0`, `pp-cpp-crypto` `v0.1.0`). Optional override: `-DPP_CPP_COMMON_SOURCE_DIR=…` / `-DPP_CPP_CRYPTO_SOURCE_DIR=…`. No system `libsodium-dev` or `nlohmann-json3-dev` required.

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

The network topology is `Beacon ← Relay ← Miners` (relays may chain). Miners configure
opaque upstream multiaddrs in `beacons[]` — same RPC whether the hop is a relay or the
terminal beacon. See [docs/amp-transport.md](docs/amp-transport.md) and
[docs/ledger-topology.md](docs/ledger-topology.md).

**Test the network** (in another terminal):
```bash
./build/app/pp-client -b status
./build/app/pp-client -m add-tx alice bob 100
./build/app/pp-client -b status
```

For detailed server setup, configuration, and troubleshooting, see **[docs/SETUP.md](docs/SETUP.md)**.

### Docker

Release tags (`release/v*`) publish a small Ubuntu 24.04 image to GHCR with all binaries:

```bash
docker pull ghcr.io/people-post/pp-ledger:latest
```

See **[deploy/README.md](deploy/README.md)** for Compose-based Beacon → Relay → Miner → HTTP setup.

## Project Structure

```
pp-ledger/
├── src/
│   ├── lib/          # Ledger common + http/cli; Value/JSON via pp-cpp-common; crypto via pp-cpp-crypto
│   ├── consensus/    # Ouroboros PoS consensus (epochs, slots, VRF leader selection)
│   ├── ledger/       # Blockchain storage and management
│   ├── chain/        # Chain, AccountBuffer, and transaction helper modules
│   ├── server/       # Beacon, Relay, and Miner server implementations
│   ├── client/       # TCP client library
│   ├── network/      # Low-level TCP networking
│   └── app/          # Executables: pp-beacon, pp-relay, pp-miner, pp-client, pp-http
├── third_party/      # Vendored third-party deps (e.g. googletest)
├── deploy/           # Docker Compose sample configs for deployment
├── scripts/          # Helper scripts
└── docs/             # Documentation
```

## Components

| Component | Description | Status |
|-----------|-------------|--------|
| **lib** | Core utilities, logging, serialization | ✅ Working |
| **consensus** | Ouroboros PoS consensus implementation | ✅ Working |
| **ledger** | Blockchain storage, ledger, wallet management | ✅ Working |
| **chain** | Chain orchestration, AccountBuffer, tx validation helpers | ✅ Working |
| **server** | Beacon, Relay, and Miner server implementations | ✅ Working |
| **client** | Client library for server communication | ✅ Working |
| **network** | TCP networking (FetchClient/Server, TcpClient/Server) | ✅ Working |
| **app** | Command-line applications (beacon, relay, miner, client, http API server) | ✅ Working |

## Documentation

- **[Setup Guide](docs/SETUP.md)** — Full beacon/miner/client setup, configuration reference, and troubleshooting
- **[Docker deployment](deploy/README.md)** — GHCR image and Compose quick start
- **[Server Architecture](src/server/SERVER.md)** — Server components, APIs, and usage guide
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
