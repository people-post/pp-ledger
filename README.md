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

### Running the Beacon Server

The beacon server requires a work directory with a `config.json` file.

**1. Create the work directory and config:**

```bash
cd build
mkdir -p beacon
cat > beacon/config.json << EOF
{
  "host": "localhost",
  "port": 8517,
  "beacons": []
}
EOF
```

**2. Start the beacon:**

```bash
./app/pp-beacon -d beacon
```

The beacon will:
- Create `beacon/ledger/` directory for blockchain data
- Create `beacon/beacon.log` for detailed logs
- Listen on `localhost:8517` for connections

### Running a Miner Server

The miner server requires a work directory with a `config.json` file containing miner details.

**1. Create the work directory and config:**

```bash
cd build
mkdir -p miner1
cat > miner1/config.json << EOF
{
  "minerId": "miner1",
  "stake": 1000000,
  "host": "localhost",
  "port": 8518,
  "beacons": ["localhost:8517"]
}
EOF
```

**2. Start the miner:**

```bash
./app/pp-miner -d miner1
```

The miner will:
- Connect to the beacon at `localhost:8517`
- Create `miner1/ledger/` directory for blockchain data
- Create `miner1/miner.log` for detailed logs
- Listen on `localhost:8518` for connections
- Automatically produce blocks when elected as slot leader (only if there are pending transactions)

### Using the Client

The client can connect to either the beacon server or miner server.

**Connect to Beacon Server:**

```bash
# Get current block ID
./app/pp-client -b current-block

# List stakeholders
./app/pp-client -b stakeholders

# Get current slot
./app/pp-client -b current-slot

# Get current epoch
./app/pp-client -b current-epoch

# Get block by ID
./app/pp-client -b block 0
```

**Connect to Miner Server:**

```bash
# Get miner status
./app/pp-client -m status

# Add a transaction
./app/pp-client -m add-tx alice bob 100

# Get pending transaction count
./app/pp-client -m pending-txs

# Manually trigger block production
./app/pp-client -m produce-block
```

**Client Options:**
- `-h <host>` - Server host (default: localhost)
- `-p <port>` - Server port
- `-h <host:port>` - Combined host and port
- `-b` - Connect to BeaconServer (default port: 8517)
- `-m` - Connect to MinerServer (default port: 8518)

### Multi-Node Setup

To run multiple nodes:

**Beacon 1:**
```bash
mkdir -p beacon1
cat > beacon1/config.json << EOF
{
  "host": "localhost",
  "port": 8517,
  "beacons": ["localhost:8527"]
}
EOF
./app/pp-beacon -d beacon1
```

**Beacon 2:**
```bash
mkdir -p beacon2
cat > beacon2/config.json << EOF
{
  "host": "localhost",
  "port": 8527,
  "beacons": ["localhost:8517"]
}
EOF
./app/pp-beacon -d beacon2
```

**Miner 1:**
```bash
mkdir -p miner1
cat > miner1/config.json << EOF
{
  "minerId": "miner1",
  "stake": 1000000,
  "host": "localhost",
  "port": 8518,
  "beacons": ["localhost:8517"]
}
EOF
./app/pp-miner -d miner1
```

**Miner 2:**
```bash
mkdir -p miner2
cat > miner2/config.json << EOF
{
  "minerId": "miner2",
  "stake": 500000,
  "host": "localhost",
  "port": 8528,
  "beacons": ["localhost:8517"]
}
EOF
./app/pp-miner -d miner2
```

### Testing the Setup

After starting a beacon and miner, you can test the system:

```bash
# Check beacon status
./app/pp-client -b -p 8517 current-block

# Check miner status
./app/pp-client -m -p 8518 status

# Add a transaction
./app/pp-client -m -p 8518 add-tx wallet1 wallet2 1000

# Wait for block production...
# Check if block was created
./app/pp-client -b -p 8517 current-block
```

### Configuration Reference

**Beacon config.json:**
```json
{
  "host": "localhost",           // Optional, default: "localhost"
  "port": 8517,                  // Optional, default: 8517
  "beacons": [                   // Optional, list of other beacon addresses
    "host1:port1",
    "host2:port2"
  ]
}
```

**Miner config.json:**
```json
{
  "minerId": "miner1",           // Required, unique identifier
  "stake": 1000000,              // Required, stake amount (affects slot leader probability)
  "host": "localhost",           // Optional, default: "localhost"
  "port": 8518,                  // Optional, default: 8518
  "beacons": [                   // Required, list of beacon addresses to connect to
    "localhost:8517"
  ]
}
```

### Troubleshooting

**"Failed to start beacon"**
- Ensure the work directory exists
- Ensure `config.json` exists in the work directory
- Check that the port is not already in use

**"Failed to open index file for writing"**
- Ensure you have write permissions in the work directory
- The ledger subdirectory will be created automatically

**"Failed to connect to beacon"**
- Ensure the beacon server is running
- Check the beacon address and port in miner's config.json
- Verify network connectivity

**Port already in use**
- Change the port number in config.json
- Or stop the process using that port

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

- **[Server Architecture](server/SERVER.md)** - Server components, APIs, and usage guide
- **[GitHub Actions Setup](docs/GITHUB_ACTIONS_SETUP.md)** - CI/CD configuration
- **[Workflow README](.github/workflows/README.md)** - GitHub Actions workflow details

## Development

For detailed information on running and configuring servers, see the [Quick Start](#quick-start) section above.

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
