# Multi-Node Server Implementation Summary

## Overview

Successfully implemented multi-node support in the Server class using TCP networking. The implementation enables distributed blockchain operation with peer-to-peer communication, consensus coordination, and state synchronization.

## Changes Made

### 1. Server.h - Header File Updates

**Added Components:**
- Forward declarations for network components (FetchClient, FetchServer)
- `NetworkConfig` structure for P2P configuration
- Network management methods (connectToPeer, getPeerCount, getConnectedPeers, isP2PEnabled)
- Overloaded `start()` method accepting NetworkConfig
- P2P member variables (p2pServer_, p2pClient_, connectedPeers_)
- P2P-specific private methods (initializeP2PNetwork, shutdownP2PNetwork, handleIncomingRequest, broadcastBlock, requestBlocksFromPeers, fetchBlocksFromPeer)

**Key Features:**
- P2P networking using TCP sockets
- Thread-safe peer management with `peersMutex_`
- Clean separation between single-node and multi-node functionality

### 2. Server.cpp - Implementation Updates

**New/Modified Methods:**

#### Overloaded start() Method
- Added `start(int port, const NetworkConfig& networkConfig)`
- Initializes P2P network when `enableP2P` is true
- Logs appropriate warnings and errors

#### Network Management
- `connectToPeer()` - Manually add peer connections (host:port format)
- `getPeerCount()` - Query number of connected peers
- `getConnectedPeers()` - Get list of peer addresses
- `isP2PEnabled()` - Check P2P status

#### Enhanced Consensus
- `shouldProduceBlock()` - Now checks slot leadership in multi-node mode
  - In P2P mode: only slot leader produces blocks
  - In single-node mode: always produces if transactions available
  
#### Block Broadcasting
- `produceBlock()` - Broadcasts new blocks to all peers
- `broadcastBlock()` - Sends block to connected peers via JSON protocol

#### State Synchronization
- `syncState()` - Fetches missing blocks from peers
- `fetchBlocksFromPeer()` - Requests blocks from specific peer
- `requestBlocksFromPeers()` - Coordinates multi-peer sync

#### P2P Infrastructure
- `initializeP2PNetwork()` - Sets up FetchClient and FetchServer
- `shutdownP2PNetwork()` - Cleanly tears down P2P components
- `handleIncomingRequest()` - Processes peer requests (get_blocks, new_block)

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Server Class                        │
├─────────────────────────────────────────────────────────┤
│  Core Components:                                        │
│  ├─ Ledger (blockchain state)                           │
│  ├─ Ouroboros Consensus                                 │
│  └─ Transaction Queue                                   │
├─────────────────────────────────────────────────────────┤
│  P2P Network (when enabled):                            │
│  ├─ FetchClient (outgoing requests)                     │
│  ├─ FetchServer (incoming requests)                     │
│  └─ Peer Management (connected peers tracking)          │
├─────────────────────────────────────────────────────────┤
│  Consensus Thread:                                       │
│  ├─ Slot Leadership Check                               │
│  ├─ Block Production (if leader)                        │
│  ├─ Block Broadcasting                                  │
│  └─ State Synchronization                               │
└─────────────────────────────────────────────────────────┘
```

## Network Protocol

### Message Types

#### 1. Get Blocks Request
```json
{
    "type": "get_blocks",
    "from_index": 100,
    "count": 10
}
```

#### 2. Blocks Response
```json
{
    "type": "blocks",
    "blocks": [
        {"index": 100, "slot": 205, "hash": "0x..."}
    ]
}
```

#### 3. New Block Broadcast
```json
{
    "type": "new_block",
    "index": 150,
    "slot": 305,
    "prev_hash": "0x...",
    "hash": "0x..."
}
```

## Usage Examples

### Single Node
```cpp
pp::Server server(2);
server.registerStakeholder("node1", 1000);
server.start(8080);
```

### Multi-Node Bootstrap Node
```cpp
pp::Server server(2);
server.registerStakeholder("bootstrap", 1000);

pp::Server::NetworkConfig config;
config.enableP2P = true;
config.nodeId = "bootstrap";
config.listenAddr = "0.0.0.0";
config.p2pPort = 9000;

server.start(8080, config);
```

### Multi-Node Joining Node
```cpp
pp::Server server(2);
server.registerStakeholder("node2", 1500);

pp::Server::NetworkConfig config;
config.enableP2P = true;
config.nodeId = "node2";
config.listenAddr = "0.0.0.0";
config.p2pPort = 9001;
config.bootstrapPeers = {"127.0.0.1:9000"};

server.start(8081, config);
```

## Build Instructions

```bash
cd pp-ledger/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## Key Design Decisions

### 1. TCP-based Networking
- Uses simple TCP sockets via TcpClient/TcpServer
- Enables both single-node and multi-node operation
- Network can be enabled/disabled via configuration

### 2. Slot Leadership
- In multi-node mode, only designated slot leader produces blocks
- Prevents fork creation and ensures consensus
- Uses Ouroboros VRF for leader selection

### 3. Thread Safety
- Transaction queue protected by `transactionQueueMutex_`
- Peer list protected by `peersMutex_`
- Network operations asynchronous
- Consensus runs in dedicated thread

### 4. Error Handling
- All network operations return `ResultOrError` types
- Graceful degradation on network failures
- Comprehensive logging for debugging

## Dependencies

### Required
- C++20 compiler
- CMake 3.15+
- Boost (system, thread, random, filesystem)
- nlohmann/json (for message serialization)

## Compatibility

- **Backward Compatible**: Existing single-node code continues to work
- **Configurable**: P2P can be enabled/disabled at runtime
- **Standard Compliant**: Uses C++20 standard features
