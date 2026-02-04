# pp-ledger

A blockchain implementation with Ouroboros consensus algorithm, written in C++20.

## Features

- ✅ **Ouroboros Consensus:** Proof-of-stake consensus with VRF-based slot leader selection
- ✅ **Blockchain & Ledger:** Complete transaction and wallet management
- ✅ **Dual Server Architecture:** Beacon servers (validators) and Miner servers (block producers)
- ✅ **Modular Architecture:** Clean separation of concerns (lib, consensus, ledger, server, client, network)
- ✅ **TCP Networking:** Simple TCP-based peer-to-peer communication
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

The beacon server is the network validator and authoritative data source. It can be initialized or mounted.

**Mode 1: Initialize a new beacon (first time setup)**

```bash
cd build
mkdir -p beacon
./app/pp-beacon -d beacon --init
```

This will:
- Create `beacon/init-config.json` with default consensus parameters if it doesn't exist
- Initialize the beacon with genesis block (block 0)
- Create `beacon/config.json` for runtime configuration

You can customize `beacon/init-config.json` before initialization:
```json
{
  "slotDuration": 5,           // Slot duration in seconds (default: 5)
  "slotsPerEpoch": 432,        // Slots per epoch (default: 432 = ~36 minutes)
  "checkpointSize": 1073741824,  // Checkpoint size in bytes (default: 1GB)
  "checkpointAge": 31536000    // Checkpoint age in seconds (default: 1 year)
}
```

**Mode 2: Mount an existing beacon**

```bash
cd build
./app/pp-beacon -d beacon
```

Optional `beacon/config.json` settings:
```json
{
  "host": "localhost",         // Optional, default: "localhost"
  "port": 8517,                // Optional, default: 8517
  "beacons": ["host:port"],    // Optional, list of other beacons
  "checkpointSize": 1073741824,  // Optional, default: 1GB
  "checkpointAge": 31536000    // Optional, default: 1 year
}
```

The beacon will:
- Create `beacon/ledger/` directory for blockchain data
- Create `beacon/beacon.log` for detailed logs
- Listen on the configured host and port (default: `localhost:8517`)
- Validate blocks (but does NOT produce blocks)

**Debug mode:**
```bash
./app/pp-beacon -d beacon --debug
```

### Running a Miner Server

The miner server produces blocks when elected as slot leader and maintains a transaction pool.

**Create the work directory and start the miner:**

```bash
cd build
mkdir -p miner1
./app/pp-miner -d miner1
```

On first run, the miner will create a default `miner1/config.json`:
```json
{
  "minerId": "miner1",
  "host": "localhost",
  "port": 8518,
  "beacons": ["127.0.0.1:8517"]
}
```

**Edit the config to customize:**
- `minerId` (required): Unique miner identifier
- `host` (optional): Listen address, default: "localhost"
- `port` (optional): Listen port, default: 8518
- `beacons` (required): List of beacon addresses to connect to

**Start the miner:**

```bash
./app/pp-miner -d miner1
```

The miner will:
- Connect to the beacon(s) specified in config
- Create `miner1/ledger/` directory for blockchain data
- Create `miner1/miner.log` for detailed logs
- Listen on the configured host and port (default: `localhost:8518`)
- Automatically produce blocks when elected as slot leader (if there are pending transactions)

**Debug mode:**
```bash
./app/pp-miner -d miner1 --debug
```

### Using the Client

The client can connect to either the beacon server or miner server to query status and send commands.

**BeaconServer Commands:**

```bash
# Get beacon status (shows current block, slot, epoch, stakeholders)
./app/pp-client -b status

# Get block by ID
./app/pp-client -b block 0

# Get slot leader for a specific slot
./app/pp-client -b slot-leader 100
```

**MinerServer Commands:**

```bash
# Get miner status (shows miner ID, stake, current slot, pending txs)
./app/pp-client -m status

# Add a transaction to the pending pool
./app/pp-client -m add-tx alice bob 100

# Manually trigger block production (for testing)
./app/pp-client -m produce-block
```

**Client Options:**
- `-h <host>` - Server host (default: localhost)
- `-h <host:port>` - Server host and port in one argument
- `-p <port>` - Server port (overrides default)
- `-b` - Connect to BeaconServer (default port: 8517)
- `-m` - Connect to MinerServer (default port: 8518)
- `--debug` - Enable debug logging

**Examples:**
```bash
# Connect to beacon on default port
./app/pp-client -b status

# Connect to beacon on custom port
./app/pp-client -b -p 8527 status

# Connect to beacon on custom host and port
./app/pp-client -h beacon.example.com:8517 -b status

# Connect to miner on default port
./app/pp-client -m status

# Connect to miner on custom host
./app/pp-client -h 192.168.1.100 -p 8518 -m add-tx wallet1 wallet2 500
```

### Multi-Node Setup

To run multiple nodes for a distributed network:

**Beacon 1:**
```bash
mkdir -p beacon1
./app/pp-beacon -d beacon1 --init  # Initialize first beacon
# Edit beacon1/config.json to add beacon2 address
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
# Copy ledger from beacon1 to ensure same blockchain state
cp -r beacon1/ledger beacon2/
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
  "host": "localhost",
  "port": 8518,
  "beacons": ["localhost:8517", "localhost:8527"]
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
  "host": "localhost",
  "port": 8528,
  "beacons": ["localhost:8517", "localhost:8527"]
}
EOF
./app/pp-miner -d miner2
```

### Testing the Setup

After starting a beacon and miner, you can test the system:

```bash
# Check beacon status
./app/pp-client -b status

# Check miner status
./app/pp-client -m status

# Add a transaction
./app/pp-client -m add-tx wallet1 wallet2 1000

# Wait for the next slot where the miner is elected as slot leader...
# The miner will automatically produce a block with the pending transaction

# Check if block was created
./app/pp-client -b status
./app/pp-client -b block 1
```

### Configuration Reference

**Beacon init-config.json** (for `--init` mode):
```json
{
  "slotDuration": 5,             // Slot duration in seconds (default: 5)
  "slotsPerEpoch": 432,          // Slots per epoch (default: 432 = ~36 min)
  "checkpointSize": 1073741824,  // Checkpoint size in bytes (default: 1GB)
  "checkpointAge": 31536000      // Checkpoint age in seconds (default: 1 year)
}
```

**Beacon config.json** (for runtime):
```json
{
  "host": "localhost",           // Optional, default: "localhost"
  "port": 8517,                  // Optional, default: 8517
  "beacons": [                   // Optional, list of other beacon addresses
    "host1:port1",
    "host2:port2"
  ],
  "checkpointSize": 1073741824,  // Optional, default: 1GB
  "checkpointAge": 31536000      // Optional, default: 1 year
}
```

**Miner config.json:**
```json
{
  "minerId": "miner1",           // Required, unique identifier
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
- For first-time setup, use `--init` flag to initialize the beacon
- For existing beacon, ensure `config.json` exists in the work directory
- Check that the port is not already in use: `netstat -tuln | grep <port>`

**"Failed to initialize beacon"**
- Ensure you have write permissions in the work directory
- Check that `init-config.json` has valid JSON format
- Review the error message in console or beacon.log

**"Failed to start miner"**
- Ensure at least one beacon is running and accessible
- Verify beacon addresses in `config.json` are correct and reachable
- Check that the miner's port is not already in use

**"Failed to connect to beacon"**
- Ensure the beacon server is running: `./app/pp-client -b status`
- Check the beacon address and port in miner's config.json
- Verify network connectivity: `telnet <beacon_host> <beacon_port>`
- Check firewall settings if running on different machines

**"Failed to open index file for writing"**
- Ensure you have write permissions in the work directory
- The ledger subdirectory will be created automatically
- Check disk space availability

**Port already in use**
- Change the port number in config.json
- Or stop the process using that port: `lsof -ti:<port> | xargs kill`

**Blocks not being produced**
- Ensure the miner has pending transactions: `./app/pp-client -m status`
- Check that the miner is registered as a stakeholder with the beacon
- Verify the miner's stake amount is registered with the beacon server
- Slot leader selection is probabilistic based on stake (may need to wait several slots)
- Review logs in `<work-dir>/miner.log` with `--debug` flag for details

## Project Structure

```
pp-ledger/
├── lib/              # Core library
│   ├── Logger        # Logging system with multiple levels
│   ├── Serialize     # Serialization utilities
│   ├── BinaryPack    # Binary packing/unpacking
│   ├── Module        # Base module class
│   ├── Service       # Base service class
│   └── Utilities     # Common utilities
├── consensus/        # Ouroboros consensus implementation
│   ├── Ouroboros     # Main consensus protocol
│   ├── EpochManager  # Epoch and slot management
│   ├── SlotTimer     # Slot timing utilities
│   └── SlotLeaderSelection  # VRF-based leader selection
├── ledger/           # Blockchain storage and management
│   ├── Ledger        # Main ledger interface
│   ├── FileStore     # File-based block storage
│   ├── DirStore      # Directory-based storage
│   ├── FileDirStore  # File + directory storage
│   └── DirDirStore   # Two-level directory storage
├── server/           # Server implementations
│   ├── Validator     # Base validator class
│   ├── Beacon        # Beacon (network validator)
│   ├── BeaconServer  # Beacon network wrapper
│   ├── Miner         # Miner (block producer)
│   ├── MinerServer   # Miner network wrapper
│   └── AccountBuffer # Account state management
├── client/           # Client library
│   └── Client        # TCP client for server communication
├── network/          # Network communication layer
│   ├── FetchClient/Server  # Request-response pattern
│   └── TcpClient/Server    # Low-level TCP
├── app/              # Command-line applications
│   ├── pp-beacon     # Beacon server executable
│   ├── pp-miner      # Miner server executable
│   └── pp-client     # Client executable
└── docs/             # Documentation
    ├── GITHUB_ACTIONS_SETUP.md
    └── README.md (in consensus/, network/, server/)
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
| **app** | Command-line applications (beacon, miner, client) | ✅ Working |

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
- **Consensus:** Ouroboros Proof-of-Stake
- **Network:** TCP-based request-response protocol

## Quick Reference

### Common Commands

```bash
# Build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)

# Initialize beacon (first time)
./app/pp-beacon -d beacon --init

# Start beacon
./app/pp-beacon -d beacon

# Start miner
./app/pp-miner -d miner1

# Query beacon status
./app/pp-client -b status

# Query miner status
./app/pp-client -m status

# Add transaction
./app/pp-client -m add-tx sender receiver 100

# Run all tests
ctest --output-on-failure
```

### Default Ports

- **Beacon Server:** 8517
- **Miner Server:** 8518

### Log Files

- Beacon: `<work-dir>/beacon.log`
- Miner: `<work-dir>/miner.log`
- Enable debug mode: `--debug` flag
