# Beacon Architecture

## Overview

The Beacon system consists of two main components that work together to provide consensus, data archival, and network coordination for the pp-ledger blockchain network.

## Components

### 1. Beacon (Core Logic)
**File:** `Beacon.h` / `Beacon.cpp`

The Beacon class is the core decision-making component responsible for:

#### Consensus Management
- Maintains Ouroboros Proof-of-Stake consensus protocol
- Tracks stakeholders and their stake amounts
- Determines slot leaders for block production
- Validates blocks according to consensus rules
- Does NOT produce blocks itself (that's the Miner's role)

#### Ledger Management
- Maintains complete blockchain history from genesis (block 0)
- Persists blocks to disk via the Ledger component
- Manages blockchain state (balances, stakes, etc.)
- Provides block retrieval by ID or range

#### Checkpoint System
The Beacon implements an intelligent checkpoint system to manage data growth:

**Checkpoint Criteria:**
- Data must exceed 1GB in size (configurable via `checkpointMinSizeBytes`)
- Blocks must be older than 1 year (configurable via `checkpointAgeSeconds`)
- Both conditions must be met simultaneously

**Checkpoint Process:**
1. Periodically evaluate blockchain size and age
2. When criteria met, create checkpoint at eligible block
3. Essential state (balances, stakes) is extracted and appended to checkpoint
4. Detailed transaction data from old blocks can be pruned
5. Block headers remain for chain continuity verification

**Benefits:**
- Reduces storage requirements for nodes
- Maintains chain integrity and verifiability
- Allows new nodes to sync from checkpoint instead of genesis
- Preserves critical state data while pruning transaction details

#### Stakeholder Registry
- Tracks all stakeholders in the network
- Maintains their network endpoints (for BeaconServer communication)
- Updates stake amounts and registers/removes stakeholders
- Synchronizes stake data with consensus protocol

#### Chain Synchronization
- Evaluates competing chains from other Beacons
- Implements longest-chain rule (Ouroboros consensus)
- Handles chain reorganization when necessary
- Ensures network consensus on canonical chain

### 2. BeaconServer (Communication Layer)
**File:** `BeaconServer.h` / `BeaconServer.cpp`

The BeaconServer handles all network communication:

#### Network Communication
- Manages FetchServer instance for TCP connections
- Handles requests from remote servers
- Processes requests from local clients
- Returns responses about chain state and stakeholders

#### Service Management
- Start/stop server lifecycle
- Configure network endpoints
- Manage connection handlers
- Log server activity

## Architecture Relationships

```
┌─────────────────────────────────────────────────────────┐
│                    BeaconServer                         │
│  (Network Communication & Request Handling)             │
│                                                         │
│  - FetchServer (TCP communication)                      │
│  - Request routing                                      │
│  - Response formatting                                  │
└────────────────┬────────────────────────────────────────┘
                 │ Uses
                 ▼
┌─────────────────────────────────────────────────────────┐
│                       Beacon                            │
│         (Core Logic & Decision Making)                  │
│                                                         │
│  ┌─────────────────────────────────────────────┐       │
│  │   Ouroboros Consensus                       │       │
│  │   - Slot leader selection                   │       │
│  │   - Block validation                        │       │
│  │   - Stake management                        │       │
│  └─────────────────────────────────────────────┘       │
│                                                         │
│  ┌─────────────────────────────────────────────┐       │
│  │   Ledger (Persistence)                      │       │
│  │   - Block storage                           │       │
│  │   - Checkpoint management                   │       │
│  │   - State persistence                       │       │
│  └─────────────────────────────────────────────┘       │
│                                                         │
│  ┌─────────────────────────────────────────────┐       │
│  │   BlockChain (In-Memory)                    │       │
│  │   - Current chain state                     │       │
│  │   - Block queries                           │       │
│  │   - Chain validation                        │       │
│  └─────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────┘
```

## Network Topology

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

### Interaction with Other Components

**Miners:**
- Miners produce blocks
- Beacons validate and archive those blocks
- Beacons broadcast validated blocks to network

**Regular Nodes:**
- Nodes can sync from Beacons
- Nodes query Beacons for historical data
- Nodes use Beacons as trusted reference points

**Other Beacons:**
- BeaconServers communicate with each other
- Share chain state and resolve conflicts
- Coordinate on checkpoint locations

## Configuration

### Beacon Configuration
```cpp
Beacon::Config config;
config.workDir = "/path/to/data";
config.checkpointMinSizeBytes = 1024ULL * 1024 * 1024; // 1GB
config.checkpointAgeSeconds = 365ULL * 24 * 3600;      // 1 year
config.slotDuration = 1;                               // 1 second
config.slotsPerEpoch = 21600;                          // 6 hours
```

### BeaconServer Configuration
```cpp
BeaconServer::Config config;
config.network.endpoint = {.address = "0.0.0.0", .port = 8517};
config.network.beacons = {"beacon1.example.com:8517", "beacon2.example.com:8517"};
```

## Usage Example

```cpp
// Initialize Beacon (core logic)
pp::Beacon beacon;
pp::Beacon::Config beaconConfig;
beaconConfig.workDir = "/var/lib/pp-ledger/beacon";
beacon.init(beaconConfig);

// Add stakeholders
pp::Beacon::Stakeholder stakeholder;
stakeholder.id = "miner1";
stakeholder.stake = 1000000;
stakeholder.endpoint = {.address = "miner1.example.com", .port = 9000};
beacon.addStakeholder(stakeholder);

// Initialize BeaconServer (communication)
pp::BeaconServer beaconServer;
beaconServer.start("/var/lib/pp-ledger/beacon");

// BeaconServer handles requests and uses Beacon for logic
// Beacon evaluates checkpoints automatically as blocks are added
// Both components work together to provide complete beacon functionality
```

## Key Design Decisions

### 1. Separation of Concerns
**Decision:** Split Beacon into core logic (Beacon) and communication (BeaconServer)

**Rationale:**
- Clear separation between decision-making and networking
- Easier to test core logic independently
- Allows alternative communication layers in the future
- Follows single responsibility principle

### 2. Checkpoint Strategy
**Decision:** Use time + size criteria for checkpoints

**Rationale:**
- Prevents premature checkpointing on small chains
- Ensures old data is truly historical before pruning
- Gives predictable data retention
- Balances storage efficiency with data availability

### 3. No Block Production
**Decision:** Beacons verify but don't produce blocks

**Rationale:**
- Prevents centralization of block production
- Separates validation from participation
- Allows Beacons to be neutral arbiters
- Distributes block production to many Miners

### 4. Composition over Inheritance
**Decision:** BeaconServer holds FetchServer instead of inheriting from it

**Rationale:**
- More flexible design
- Clearer ownership and lifecycle
- Easier to replace communication layer
- Better encapsulation

## Future Enhancements

1. **Distributed Checkpointing:** Coordinate checkpoint locations across Beacons
2. **Checkpoint Verification:** Add cryptographic proofs for checkpoint validity
3. **State Snapshots:** Include full state snapshots in checkpoints
4. **Pruning Automation:** Automatically prune data after checkpoint creation
5. **Checkpoint Distribution:** Serve checkpoints to new nodes for fast sync
6. **Byzantine Fault Tolerance:** Add BFT mechanisms for Beacon consensus
7. **Dynamic Beacon Set:** Allow stakeholder voting on Beacon membership

## Performance Considerations

- Checkpoint evaluation runs periodically (not on every block)
- Block validation is lightweight (consensus rules only)
- Stakeholder registry uses mutex for thread-safety
- Ledger operations are async-safe
- BeaconServer uses separate thread for network I/O

## Security Considerations

- Beacons must be trusted nodes (carefully selected)
- Private keys for Beacon identity should be secure
- Network endpoints should use TLS in production
- Checkpoint integrity must be cryptographically verified
- Stake updates should be authenticated
