# Server Architecture Overview

## Components

The pp-ledger server architecture consists of two main server types:

1. **BeaconServer** - Network interface for Beacon (validator/archiver)
2. **MinerServer** - Network interface for Miner (block producer)

Both servers follow a composition pattern, wrapping their respective core logic classes (Beacon and Miner) with network communication capabilities via FetchServer.

## BeaconServer

### Purpose
BeaconServer provides the network interface for the Beacon core, which handles block validation, checkpoint management, and serves as the authoritative data source.

### Key Features

- **Server Registration**: Accepts registration from miners and other servers
- **Heartbeat Handling**: Tracks active servers via periodic heartbeat messages
- **Block Management**: GET/ADD blocks, query current block ID
- **Checkpoint Management**: List/evaluate checkpoints
- **Stakeholder Management**: Add/remove/update stakeholders
- **Consensus Queries**: Query slot leaders, current epoch, current slot

### Configuration (config.json)

```json
{
  "host": "localhost",
  "port": 8517,
  "beacons": ["127.0.0.1:8517", "127.0.0.1:8518"]
}
```

### API Endpoints

All requests/responses use JSON format:

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

### Usage Example

```cpp
pp::BeaconServer beaconServer;

// Start server with data directory
if (beaconServer.start("/data/beacon1")) {
    // Server running
    
    // Access underlying beacon
    auto& beacon = beaconServer.getBeacon();
    uint64_t currentBlock = beacon.getCurrentBlockId();
    
    // Wait for shutdown signal...
    
    beaconServer.stop();
}
```

## MinerServer

### Purpose
MinerServer provides the network interface for the Miner core, which handles block production, transaction management, and checkpoint synchronization.

### Key Features

- **Transaction Pool**: Accept transactions into pending pool
- **Block Production**: Automatic block production when slot leader
- **Block Management**: GET/ADD blocks from network
- **Mining Control**: Query mining status, manually trigger production
- **Checkpoint Sync**: Reinitialize from checkpoints when out of date
- **Status Queries**: Get miner status, pending transactions, etc.

### Configuration (config.json)

```json
{
  "minerId": "miner1",
  "stake": 1000000,
  "host": "localhost",
  "port": 8518,
  "beacons": ["127.0.0.1:8517"]
}
```

### API Endpoints

All requests/responses use JSON format:

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

### Usage Example

```cpp
pp::MinerServer minerServer;

// Start server with data directory
if (minerServer.start("/data/miner1")) {
    // Server running with automatic block production
    
    // Access underlying miner
    auto& miner = minerServer.getMiner();
    
    // Add transaction
    Ledger::Transaction tx;
    tx.fromWallet = "alice";
    tx.toWallet = "bob";
    tx.amount = 100;
    miner.addTransaction(tx);
    
    // Wait for shutdown signal...
    
    minerServer.stop();
}
```

### Block Production Loop

MinerServer runs an automatic block production loop in a background thread:

1. Check if current slot leader
2. If yes, produce block with pending transactions
3. Broadcast block (TODO: implement broadcast to beacons)
4. Sleep briefly
5. Repeat

The loop runs continuously while the server is active.

## Architecture Pattern

Both servers follow the same design pattern:

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
│  (Beacon/Miner)  │  - Ledger management
│                  │  - Block/Transaction processing
└──────────────────┘
```

### Benefits of Composition

1. **Separation of Concerns**: Network logic separated from business logic
2. **Testability**: Core logic can be tested independently
3. **Flexibility**: Multiple servers can use the same core
4. **Maintainability**: Clear boundaries between components

## Network Topology

```
┌─────────────┐          ┌─────────────┐          ┌─────────────┐
│   Beacon    │◄────────►│   Beacon    │◄────────►│   Beacon    │
│  (8517)     │          │  (8527)     │          │  (8537)     │
└──────▲──────┘          └──────▲──────┘          └──────▲──────┘
       │                        │                        │
       │                        │                        │
       │ Blocks/Validation      │                        │
       │                        │                        │
┌──────┴──────┐          ┌──────┴──────┐          ┌──────┴──────┐
│   Miner     │          │   Miner     │          │   Miner     │
│  (8518)     │          │  (8528)     │          │  (8538)     │
└─────────────┘          └─────────────┘          └─────────────┘
```

- **Beacons**: Form a network of validators, sync with each other
- **Miners**: Produce blocks, submit to beacons for validation
- **Beacons validate**: Accept/reject blocks from miners
- **Beacons create checkpoints**: When criteria met (1GB + 1 year)
- **Miners sync from checkpoints**: Reinitialize when falling behind

## Error Handling

Both servers return JSON error responses:

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

## Thread Safety

- **BeaconServer**: Thread-safe access to active servers list
- **MinerServer**: Thread-safe transaction pool and state management
- Both servers handle concurrent requests via FetchServer

## Future Enhancements

### BeaconServer
- Block broadcast to other beacons
- Chain synchronization between beacons
- Checkpoint distribution
- Miner authorization and authentication

### MinerServer
- Block broadcast to beacons after production
- Automatic checkpoint synchronization
- Transaction fee management
- Mining pool support

### Both
- TLS/SSL encryption
- Authentication and authorization
- Rate limiting
- Request logging and metrics
- WebSocket support for real-time updates
