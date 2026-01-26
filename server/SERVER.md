# Server Architecture

This document describes the server architecture for the pp-ledger blockchain system.

## Table of Contents

1. [Overview](#overview)
2. [Components](#components)
3. [Beacon Architecture](#beacon-architecture)
4. [Miner Architecture](#miner-architecture)
5. [Network Topology](#network-topology)
6. [Configuration](#configuration)
7. [API Reference](#api-reference)
8. [Usage Examples](#usage-examples)

## Overview

The pp-ledger server architecture consists of three main components:

1. **Validator** - Base class providing common block validation and chain management
2. **Beacon** - Network validator and data archiver (extends Validator)
3. **Miner** - Block producer (extends Validator)

Each component has a corresponding server wrapper (*Server classes) that handles network communication via TCP.

### Architecture Pattern

```
┌──────────────────┐
│   *Server        │  - Network communication (FetchServer)
│                  │  - Request routing and JSON handling
│                  │  - Configuration loading
└────────┬─────────┘
         │ owns
         ▼
┌──────────────────┐
│  Core Logic      │  - Consensus (Beacon/Miner)
│  (Validator)     │  - Ledger management
│                  │  - Block/Transaction processing
└──────────────────┘
```

**Benefits of this composition:**

- **Separation of Concerns**: Network logic separated from business logic
- **Testability**: Core logic can be tested independently
- **Flexibility**: Multiple servers can use the same core
- **Maintainability**: Clear boundaries between components

## Components

### Validator (Base Class)

The Validator class provides common functionality for both Beacon and Miner:

**Responsibilities:**
- Block validation
- Chain management (in-memory BlockChain)
- Consensus integration (Ouroboros)
- Ledger operations (persistent storage)
- Base configuration

**Key Methods:**
- `getBlock(blockId)` - Retrieve a block by ID
- `addBlockBase(block)` - Add a validated block to the chain
- `getCurrentBlockId()` - Get the ID of the current block
- `getCurrentSlot()` - Get the current consensus slot
- `getCurrentEpoch()` - Get the current consensus epoch

### BeaconServer + Beacon

**Purpose:** Network validator and authoritative data source

**Beacon (Core Logic) Responsibilities:**
- Maintain full blockchain history from genesis (block 0)
- Manage Ouroboros consensus protocol
- Track stakeholders and their stake amounts
- Determine checkpoint locations for data pruning
- Validate blocks (but does NOT produce them)
- Serve as authoritative data source for the network

**Checkpoint System:**

Beacons implement an intelligent checkpoint system to manage data growth:

- **Criteria**: Data must exceed 1GB AND blocks must be older than 1 year
- **Process**: When criteria are met, create checkpoint with essential state (balances, stakes)
- **Benefits**: Reduces storage requirements while maintaining chain integrity
- **New Node Sync**: Allows nodes to sync from checkpoint instead of genesis

**BeaconServer (Communication Layer) Responsibilities:**
- Handle network requests via FetchServer (TCP)
- Route requests to Beacon core logic
- Manage connections from miners and clients
- Return chain state and stakeholder information

### MinerServer + Miner

**Purpose:** Block producer

**Miner (Core Logic) Responsibilities:**
- Produce blocks when selected as slot leader
- Maintain transaction pool for pending transactions
- Sync with network to get latest blocks
- Reinitialize from checkpoints when needed
- Validate incoming blocks from other miners

**Transaction Pool:**
- FIFO queue of pending transactions
- Configurable maximum size
- Thread-safe access
- Transactions removed when included in blocks

**MinerServer (Communication Layer) Responsibilities:**
- Handle network requests via FetchServer (TCP)
- Automatic block production loop in background thread
- Accept transactions into pending pool
- Route requests to Miner core logic
- Provide mining status and control

## Beacon Architecture

### Beacon Role in Network

Beacons serve a unique role in the pp-ledger network:

**Limited in Number:**
- Beacons are intentionally few (e.g., 5-10 globally)
- They are trusted, well-resourced nodes
- Run by network founders or elected by stakeholders

**Responsibilities:**
1. **Data Archival:** Maintain complete blockchain history
2. **Block Verification:** Validate all blocks (but don't produce them)
3. **Checkpointing:** Determine and create checkpoints
4. **Authority:** Serve as reference for chain state
5. **Coordination:** Help other nodes sync and validate

**What Beacons DON'T Do:**
- They do NOT produce blocks (that's the Miners' job)
- They do NOT participate in slot leader selection
- They do NOT mine or earn block rewards

### Beacon Configuration

**File:** `config.json` in work directory

```json
{
  "host": "localhost",
  "port": 8517,
  "beacons": ["host1:port1", "host2:port2"]
}
```

**Fields:**
- `host` (optional): Listen address, default: "localhost"
- `port` (optional): Listen port, default: 8517
- `beacons` (optional): List of other beacon addresses for network coordination

### Beacon API Endpoints

All requests/responses use JSON format.

#### Server Management
```json
// Register server
{"type": "register", "address": "host:port"}

// Heartbeat
{"type": "heartbeat", "address": "host:port"}

// Query active servers
{"type": "query"}
```

#### Block Operations
```json
// Get block
{"type": "block", "action": "get", "blockId": 123}

// Add block
{"type": "block", "action": "add", "block": {...}}

// Get current block ID
{"type": "block", "action": "current"}
```

#### Checkpoint Operations
```json
// List checkpoints
{"type": "checkpoint", "action": "list"}

// Get current checkpoint
{"type": "checkpoint", "action": "current"}

// Evaluate checkpoints
{"type": "checkpoint", "action": "evaluate"}
```

#### Stakeholder Operations
```json
// List stakeholders
{"type": "stakeholder", "action": "list"}

// Add stakeholder
{"type": "stakeholder", "action": "add", "stakeholder": {"id": "...", "stake": 1000, ...}}

// Remove stakeholder
{"type": "stakeholder", "action": "remove", "id": "stakeholder1"}

// Update stake
{"type": "stakeholder", "action": "updateStake", "id": "stakeholder1", "stake": 2000}
```

#### Consensus Queries
```json
// Get current slot
{"type": "consensus", "action": "currentSlot"}

// Get current epoch
{"type": "consensus", "action": "currentEpoch"}

// Get slot leader
{"type": "consensus", "action": "slotLeader", "slot": 123}
```

## Miner Architecture

### Miner Configuration

**File:** `config.json` in work directory

```json
{
  "minerId": "miner1",
  "stake": 1000000,
  "host": "localhost",
  "port": 8518,
  "beacons": ["127.0.0.1:8517"]
}
```

**Fields:**
- `minerId` (required): Unique miner identifier
- `stake` (required): Stake amount (affects slot leader probability)
- `host` (optional): Listen address, default: "localhost"
- `port` (optional): Listen port, default: 8518
- `beacons` (required): List of beacon addresses to connect to

### Miner API Endpoints

All requests/responses use JSON format.

#### Status
```json
// Get miner status
{"type": "status"}

// Response:
{
  "status": "ok",
  "minerId": "miner1",
  "stake": 1000000,
  "currentBlockId": 456,
  "currentSlot": 789,
  "currentEpoch": 3,
  "pendingTransactions": 42,
  "isSlotLeader": true
}
```

#### Transaction Operations
```json
// Add transaction
{"type": "transaction", "action": "add", "transaction": {"from": "alice", "to": "bob", "amount": 100}}

// Get pending transaction count
{"type": "transaction", "action": "count"}

// Clear transaction pool
{"type": "transaction", "action": "clear"}
```

#### Block Operations
```json
// Get block
{"type": "block", "action": "get", "blockId": 123}

// Add block
{"type": "block", "action": "add", "block": {...}}

// Get current block ID
{"type": "block", "action": "current"}
```

#### Mining Operations
```json
// Produce block (manual trigger)
{"type": "mining", "action": "produce"}

// Check if should produce
{"type": "mining", "action": "shouldProduce"}
```

#### Checkpoint Operations
```json
// Reinitialize from checkpoint
{"type": "checkpoint", "action": "reinit", "checkpoint": {"blockId": 1000, "stateData": [...]}}

// Check if out of date
{"type": "checkpoint", "action": "isOutOfDate", "checkpointId": 1000}
```

#### Consensus Queries
```json
// Get current slot
{"type": "consensus", "action": "currentSlot"}

// Get current epoch
{"type": "consensus", "action": "currentEpoch"}

// Check if slot leader
{"type": "consensus", "action": "isSlotLeader"}
// Or for specific slot:
{"type": "consensus", "action": "isSlotLeader", "slot": 123}
```

### Block Production Loop

MinerServer runs an automatic block production loop in a background thread:

1. Check if current slot leader
2. If yes, produce block with pending transactions
3. Broadcast block to network
4. Sleep briefly
5. Repeat

The loop runs continuously while the server is active.

### Checkpoint Reinitialization

**Problem:** New miners or miners that have been offline may have outdated ledger data.

**Solution:** Reinitialize from a recent checkpoint instead of syncing from genesis.

**Process:**
1. Load checkpoint state (balances, stakes, etc.)
2. Clear existing transaction pool
3. Rebuild ledger from checkpoint block ID
4. Prune old blocks before checkpoint
5. Resume normal operation from checkpoint

**When to Reinitialize:**

Miner is considered out of date if:
- Remote checkpoint is > 1000 blocks ahead
- Local data is corrupted or inconsistent
- Explicit reinitialization requested

## Network Topology

### Multi-Node Architecture

```
┌─────────────┐          ┌─────────────┐          ┌─────────────┐
│   Beacon    │◄────────►│   Beacon    │◄────────►│   Beacon    │
│  (8517)     │          │  (8527)     │          │  (8537)     │
└──────▲──────┘          └──────▲──────┘          └──────▲──────┘
       │                        │                        │
       │ Blocks/Validation      │                        │
       │                        │                        │
┌──────┴──────┐          ┌──────┴──────┐          ┌──────┴──────┐
│   Miner     │          │   Miner     │          │   Miner     │
│  (8518)     │          │  (8528)     │          │  (8538)     │
└─────────────┘          └─────────────┘          └─────────────┘
```

**Interaction Flow:**
- **Miners** produce blocks based on stake
- **Beacons** validate and archive blocks from miners
- **Beacons** form a network and sync with each other
- **Beacons** create checkpoints when criteria are met (1GB + 1 year)
- **Miners** sync from checkpoints when falling behind

### Consensus Flow

```
Time (Slots) ───────────────────────────────────────────────────────────►

Slot N:     Miner 1 (Leader)     Miner 2              Miner 3
            │                    │                    │
            │ Check: Am I        │ Check: Am I        │ Check: Am I
            │ leader? YES        │ leader? NO         │ leader? NO
            │                    │                    │
            ▼                    │                    │
      ┌──────────┐              │                    │
      │ Produce  │              │                    │
      │  Block   │              │                    │
      └──────────┘              │                    │
            │                    │                    │
            │ Broadcast          │                    │
            ├───────────────────►│                    │
            │                    │                    │
            └───────────────────────────────────────►│
            │                    │                    │
            │                    ▼                    ▼
            │              ┌──────────┐         ┌──────────┐
            │              │ Validate │         │ Validate │
            │              │  Block   │         │  Block   │
            │              └──────────┘         └──────────┘
            │                    │                    │
            │                    ▼                    ▼
            │              ┌──────────┐         ┌──────────┐
            │              │   Add    │         │   Add    │
            │              │  Block   │         │  Block   │
            │              └──────────┘         └──────────┘
            │                    │                    │
            ▼                    ▼                    ▼
         [Block N]            [Block N]           [Block N]
```

## Configuration

### Validator Base Configuration

Common configuration for both Beacon and Miner:

```cpp
struct BaseConfig {
    std::string workDir;              // Directory for data storage
    uint64_t slotDuration = 1;        // Duration of each slot (seconds)
    uint64_t slotsPerEpoch = 21600;   // Number of slots per epoch (~6 hours)
};
```

### Beacon-Specific Configuration

```cpp
struct Config : public BaseConfig {
    uint64_t checkpointMinSizeBytes = 1024ULL * 1024 * 1024; // 1GB default
    uint64_t checkpointAgeSeconds = 365ULL * 24 * 3600;      // 1 year default
};
```

### Miner-Specific Configuration

```cpp
struct Config : public BaseConfig {
    std::string minerId;                       // Unique miner identifier
    uint64_t stake;                            // Miner's stake in the network
    size_t maxPendingTransactions = 10000;     // Max size of transaction pool
    size_t maxTransactionsPerBlock = 100;      // Max transactions per block
};
```

## API Reference

### Error Handling

All servers return JSON error responses on failure:

```json
{
  "error": "description of error"
}
```

Success responses include:

```json
{
  "status": "ok",
  ...additional fields...
}
```

### Thread Safety

- **BeaconServer**: Thread-safe access to active servers list
- **MinerServer**: Thread-safe transaction pool and state management
- **Both servers**: Handle concurrent requests via FetchServer

## Usage Examples

### Running a Beacon Server

1. **Create the work directory and config:**

```bash
mkdir -p beacon
cat > beacon/config.json << EOF
{
  "host": "localhost",
  "port": 8517,
  "beacons": []
}
EOF
```

2. **Start the beacon:**

```bash
./build/app/pp-beacon -d beacon
```

The beacon will:
- Create `beacon/ledger/` directory for blockchain data
- Create `beacon/beacon.log` for detailed logs
- Listen on `localhost:8517` for connections

### Running a Miner Server

1. **Create the work directory and config:**

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
```

2. **Start the miner:**

```bash
./build/app/pp-miner -d miner1
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
./build/app/pp-client -b current-block

# List stakeholders
./build/app/pp-client -b stakeholders

# Get current slot
./build/app/pp-client -b current-slot

# Get current epoch
./build/app/pp-client -b current-epoch

# Get block by ID
./build/app/pp-client -b block 0
```

**Connect to Miner Server:**

```bash
# Get miner status
./build/app/pp-client -m status

# Add a transaction
./build/app/pp-client -m add-tx alice bob 100

# Get pending transaction count
./build/app/pp-client -m pending-txs

# Manually trigger block production
./build/app/pp-client -m produce-block
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
./build/app/pp-beacon -d beacon1
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
./build/app/pp-beacon -d beacon2
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
./build/app/pp-miner -d miner1
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
./build/app/pp-miner -d miner2
```

### Testing the Setup

After starting a beacon and miner, you can test the system:

```bash
# Check beacon status
./build/app/pp-client -b -p 8517 current-block

# Check miner status
./build/app/pp-client -m -p 8518 status

# Add a transaction
./build/app/pp-client -m -p 8518 add-tx wallet1 wallet2 1000

# Wait for block production...
# Check if block was created
./build/app/pp-client -b -p 8517 current-block
```

### Stopping the Servers

Press `Ctrl+C` in the terminal where the server is running to stop it gracefully.

## Storage Management

### Work Directory Structure

```
workDir/
  ledger/
    blocks/
      00000/
      00001/
      ...
    index.dat
    checkpoints.dat
  config.json
  beacon.log (or miner.log)
```

### Ledger Persistence

- Blocks stored in `DirDirStore` backend
- Automatic file rotation when files reach size limit
- Recursive directory structure for scalability
- Index maintained for fast block lookup

## Troubleshooting

### "Failed to start beacon"
- Ensure the work directory exists
- Ensure `config.json` exists in the work directory
- Check that the port is not already in use

### "Failed to open index file for writing"
- Ensure you have write permissions in the work directory
- The ledger subdirectory will be created automatically

### "Failed to connect to beacon"
- Ensure the beacon server is running
- Check the beacon address and port in miner's config.json
- Verify network connectivity

### Port already in use
- Change the port number in config.json
- Or stop the process using that port

## Security Considerations

1. **Beacon Trust**: Beacons must be trusted nodes (carefully selected)
2. **Private Keys**: Private keys for node identity should be secure
3. **Network Security**: Network endpoints should use TLS in production
4. **Checkpoint Integrity**: Checkpoint integrity must be cryptographically verified
5. **Stake Validation**: Verify miner has claimed stake before producing
6. **Timestamp Manipulation**: Prevent miners from manipulating block times
7. **Transaction Validation**: Verify transactions before including in blocks

## Future Enhancements

### BeaconServer
- Block broadcast to other beacons
- Chain synchronization between beacons
- Checkpoint distribution
- Miner authorization and authentication
- Distributed checkpointing coordination
- Byzantine Fault Tolerance mechanisms

### MinerServer
- Block broadcast to beacons after production
- Automatic checkpoint synchronization
- Transaction fee management
- Mining pool support
- Fee-based transaction prioritization
- Anti-spam measures for transaction pool

### Both
- TLS/SSL encryption
- Authentication and authorization
- Rate limiting
- Request logging and metrics
- WebSocket support for real-time updates
- Peer discovery mechanisms
- Advanced routing algorithms
- Network partition handling

## References

- **Ouroboros Paper**: [Ouroboros: A Provably Secure Proof-of-Stake Blockchain Protocol](https://eprint.iacr.org/2016/889.pdf)
- **Consensus Module**: `/consensus/`
- **Ledger Module**: `/ledger/`
- **Network Module**: `/network/`
