# Multi-Node Support in Server Class

## Overview

The Server class supports multi-node blockchain networking using TCP sockets. This enables:

- **Peer-to-peer communication** between blockchain nodes
- **Distributed consensus** using the Ouroboros protocol
- **Block propagation** across the network
- **State synchronization** between nodes
- **Slot leader selection** in multi-node environments

## Features

### 1. Network Configuration

The `NetworkConfig` structure provides flexible P2P network configuration:

```cpp
struct NetworkConfig {
    bool enableP2P = false;                      // Enable P2P networking
    std::string nodeId;                          // Unique node identifier
    std::vector<std::string> bootstrapPeers;     // Bootstrap peer host:port addresses
    std::string listenAddr = "0.0.0.0";          // P2P listen address
    uint16_t p2pPort = 9000;                     // P2P listen port
    uint16_t maxPeers = 50;                      // Maximum peer connections
};
```

### 2. P2P Networking

The server includes:

- **FetchClient**: Sends requests to remote peers
- **FetchServer**: Handles incoming requests from peers
- **Peer Management**: Tracks connected peers and manages connections

### 3. Consensus Integration

The multi-node implementation integrates with Ouroboros consensus:

- **Slot Leadership**: Only the designated slot leader produces blocks
- **Block Validation**: All nodes validate incoming blocks
- **Stake Registration**: Nodes register their stake for leader selection
- **Epoch Management**: Synchronized across all nodes

### 4. Block Propagation

New blocks are automatically broadcast to all connected peers:

1. Slot leader creates a new block
2. Block is validated locally
3. Block is serialized and broadcast via P2P network
4. Peers receive, validate, and add block to their ledger

### 5. State Synchronization

Nodes periodically synchronize state with peers:

1. Request blocks from connected peers
2. Validate received blocks using consensus rules
3. Update local blockchain if peer chain is better
4. Maintain consistency across the network

## Usage

### Single Node (No P2P)

```cpp
pp::Server server(2);  // Difficulty 2
server.registerStakeholder("node1", 1000);
server.setSlotDuration(5);
server.start(8080);  // HTTP port only
```

### Multi-Node Network

#### Bootstrap Node

```cpp
pp::Server server(2);
server.registerStakeholder("bootstrap", 1000);
server.setSlotDuration(5);

pp::Server::NetworkConfig config;
config.enableP2P = true;
config.nodeId = "bootstrap";
config.listenAddr = "0.0.0.0";
config.p2pPort = 9000;
// No bootstrap peers - this is the first node

server.start(8080, config);
```

#### Joining Node

```cpp
pp::Server server(2);
server.registerStakeholder("node2", 1500);
server.setSlotDuration(5);

pp::Server::NetworkConfig config;
config.enableP2P = true;
config.nodeId = "node2";
config.listenAddr = "0.0.0.0";
config.p2pPort = 9001;
config.bootstrapPeers = {"127.0.0.1:9000"};

server.start(8081, config);
```

### Manual Peer Connection

```cpp
server.connectToPeer("192.168.1.100:9000");
```

### Query Network Status

```cpp
bool p2pEnabled = server.isP2PEnabled();
size_t peerCount = server.getPeerCount();
std::vector<std::string> peers = server.getConnectedPeers();
```

## API Reference

### Constructor

```cpp
explicit Server(uint32_t blockchainDifficulty = 2);
```

Creates a new server instance with the specified blockchain difficulty.

### Start Server

```cpp
bool start(int port);
bool start(int port, const NetworkConfig& networkConfig);
```

Starts the server on the specified port. The second overload enables P2P networking.

### Network Management

```cpp
void connectToPeer(const std::string& hostPort);
size_t getPeerCount() const;
std::vector<std::string> getConnectedPeers() const;
bool isP2PEnabled() const;
```

### Consensus Management

```cpp
void registerStakeholder(const std::string& id, uint64_t stake);
void setSlotDuration(uint64_t seconds);
```

### Transaction Management

```cpp
void submitTransaction(const std::string& transaction);
size_t getPendingTransactionCount() const;
```

### State Queries

```cpp
uint64_t getCurrentSlot() const;
uint64_t getCurrentEpoch() const;
size_t getBlockCount() const;
Roe<int64_t> getBalance(const std::string& walletId) const;
```

## Network Protocol

### Message Format

Messages are exchanged in JSON format:

#### Get Blocks Request

```json
{
    "type": "get_blocks",
    "from_index": 100,
    "count": 10
}
```

#### Blocks Response

```json
{
    "type": "blocks",
    "blocks": [
        {"index": 100, "slot": 205, "hash": "0x..."},
        {"index": 101, "slot": 206, "hash": "0x..."}
    ]
}
```

#### New Block Broadcast

```json
{
    "type": "new_block",
    "index": 150,
    "slot": 305,
    "prev_hash": "0x...",
    "hash": "0x..."
}
```

## Building

### Prerequisites

- C++20 compiler
- CMake 3.15+
- Boost (system, thread)
- nlohmann/json

### Build Commands

```bash
cd pp-ledger
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run example
./server/multi_node_example
```

## Architecture

### Components

```
Server
├── Ledger (blockchain state)
├── Ouroboros (consensus)
├── P2P Network (when enabled)
│   ├── FetchClient (outgoing requests)
│   └── FetchServer (incoming requests)
└── Consensus Thread
    ├── Block Production (if slot leader)
    ├── Block Broadcasting (to peers)
    └── State Synchronization (from peers)
```

### Consensus Loop

The consensus loop runs continuously and performs:

1. **Check Slot Leadership**: Determine if this node should produce a block
2. **Produce Block**: Create block from pending transactions (if slot leader)
3. **Broadcast Block**: Send new block to all peers
4. **Sync State**: Request missing blocks from peers
5. **Sleep**: Wait before next iteration

### Thread Safety

- Transaction queue protected by `transactionQueueMutex_`
- Peer list protected by `peersMutex_`
- Consensus operations run in dedicated thread
- Network callbacks handled asynchronously

## Troubleshooting

### Peers Not Connecting

- Verify peer addresses are in correct host:port format
- Check firewall settings allow P2P port
- Ensure network connectivity

### Blocks Not Propagating

- Check node is registered as stakeholder
- Verify slot duration is synchronized
- Check network server is running
- Review logs for broadcast errors

### Runtime Errors

- Check server logs
- Verify genesis time is synchronized
- Ensure node IDs are unique
- Check for port conflicts

## Future Enhancements

### Short Term

- [ ] Complete block serialization/deserialization
- [ ] Implement peer exchange protocol
- [ ] Add authentication and encryption
- [ ] Persistent peer storage

### Medium Term

- [ ] Implement peer discovery
- [ ] Add bandwidth management
- [ ] Implement block validation caching
- [ ] Add metrics and monitoring

### Long Term

- [ ] Light client support
- [ ] Cross-shard communication
- [ ] Advanced routing algorithms
- [ ] Network partition handling
