# Multi-Node Support in Server Class

## Overview

The Server class now supports multi-node blockchain networking using the libp2p library. This enables:

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
    std::vector<std::string> bootstrapPeers;     // Bootstrap peer multiaddresses
    std::string listenAddr = "/ip4/0.0.0.0/tcp/9000";  // P2P listen address
    uint16_t maxPeers = 50;                      // Maximum peer connections
};
```

### 2. P2P Networking

The server includes:

- **libp2p Host**: Manages peer connections and network protocols
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
config.listenAddr = "/ip4/0.0.0.0/tcp/9000";
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
config.listenAddr = "/ip4/0.0.0.0/tcp/9001";
config.bootstrapPeers = {
    "/ip4/127.0.0.1/tcp/9000/p2p/QmBootstrapPeerID"
};

server.start(8081, config);
```

### Manual Peer Connection

```cpp
server.connectToPeer("/ip4/192.168.1.100/tcp/9000/p2p/QmPeerID");
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

**Parameters:**
- `port`: HTTP server port
- `networkConfig`: P2P network configuration

**Returns:** `true` if started successfully, `false` otherwise

### Network Management

```cpp
void connectToPeer(const std::string& multiaddr);
```

Manually connect to a peer using a libp2p multiaddress.

```cpp
size_t getPeerCount() const;
```

Returns the number of connected peers.

```cpp
std::vector<std::string> getConnectedPeers() const;
```

Returns a list of connected peer multiaddresses.

```cpp
bool isP2PEnabled() const;
```

Returns `true` if P2P networking is enabled and operational.

### Consensus Management

```cpp
void registerStakeholder(const std::string& id, uint64_t stake);
```

Registers a stakeholder for Ouroboros consensus.

```cpp
void setSlotDuration(uint64_t seconds);
```

Sets the slot duration for consensus.

### Transaction Management

```cpp
void submitTransaction(const std::string& transaction);
```

Submits a transaction to the transaction pool.

```cpp
size_t getPendingTransactionCount() const;
```

Returns the number of pending transactions.

### State Queries

```cpp
uint64_t getCurrentSlot() const;
uint64_t getCurrentEpoch() const;
size_t getBlockCount() const;
Roe<int64_t> getBalance(const std::string& walletId) const;
```

Query current blockchain state.

## Network Protocol

### Protocol Identifier

The server uses the protocol `/pp-ledger/sync/1.0.0` for P2P communication.

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

## Building with P2P Support

### Prerequisites

- libp2p library installed (required)
- C++20 compiler
- CMake 3.15+

### Build Commands

```bash
cd /workspaces/pp-ledger
mkdir build && cd build

# Configure with libp2p (required)
cmake -DLIBP2P_ROOT=/path/to/libp2p-install ..

# Build
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
├── P2P Network (required)
│   ├── libp2p::Host (peer management)
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

## Implementation Notes

### Block Production in Multi-Node Mode

When P2P is enabled, only the slot leader produces blocks:

```cpp
bool Server::shouldProduceBlock() const {
    if (getPendingTransactionCount() == 0) {
        return false;
    }
    
    if (networkConfig_.enableP2P && !networkConfig_.nodeId.empty()) {
        uint64_t currentSlot = ukpConsensus_->getCurrentSlot();
        auto slotLeaderResult = ukpConsensus_->getSlotLeader(currentSlot);
        
        if (slotLeaderResult && slotLeaderResult.value() == networkConfig_.nodeId) {
            return true;  // This node is the slot leader
        }
        return false;  // Another node is the slot leader
    }
    
    return true;  // Single-node mode: always produce
}
```

### State Synchronization

Nodes sync state by:

1. Requesting blocks from peers starting at local block height
2. Validating received blocks against consensus rules
3. Adding valid blocks to the local ledger
4. Rejecting invalid blocks

### Peer Discovery

Currently supports:

- **Bootstrap peers**: Manually configured initial peers
- **Manual connections**: Connect to specific peers via `connectToPeer()`

Future enhancements could include:

- Peer exchange (gossip protocol)
- DHT-based peer discovery
- mDNS for local network discovery

## Testing

### Unit Tests

Test individual components:

```bash
cd build
ctest -R test_server
```

### Integration Tests

Test multi-node scenarios:

1. Start multiple server instances
2. Connect nodes via bootstrap peers
3. Submit transactions to different nodes
4. Verify block propagation
5. Verify state consistency

### Example Test Scenario

```cpp
// Node 1 (bootstrap)
Server server1(2);
server1.registerStakeholder("node1", 1000);
NetworkConfig config1{true, "node1", {}, "/ip4/0.0.0.0/tcp/9000"};
server1.start(8081, config1);

// Node 2 (connects to node1)
Server server2(2);
server2.registerStakeholder("node2", 1500);
NetworkConfig config2{true, "node2", {"/ip4/127.0.0.1/tcp/9000"}, "/ip4/0.0.0.0/tcp/9001"};
server2.start(8082, config2);

// Submit transaction on node1
server1.submitTransaction("tx1");

// Wait for propagation
std::this_thread::sleep_for(std::chrono::seconds(10));

// Verify both nodes have same block count
assert(server1.getBlockCount() == server2.getBlockCount());
```

## Troubleshooting

### Peers Not Connecting

- Verify bootstrap peer multiaddresses are correct
- Check firewall settings allow P2P port
- Ensure libp2p is built and linked correctly

### Blocks Not Propagating

- Check node is registered as stakeholder
- Verify slot duration is synchronized
- Check network server is running
- Review logs for broadcast errors

### Build Errors

- Ensure `USE_LIBP2P` is set in CMake
- Verify `LIBP2P_ROOT` points to correct installation
- Check C++20 compiler is available
- Verify all dependencies (Boost, OpenSSL) are installed

### Runtime Errors

- Check logs in `~/.pp-ledger/logs/`
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

- [ ] Implement DHT for peer discovery
- [ ] Add bandwidth management
- [ ] Implement block validation caching
- [ ] Add metrics and monitoring

### Long Term

- [ ] Light client support
- [ ] Cross-shard communication
- [ ] Advanced routing algorithms
- [ ] Network partition handling

## References

- [libp2p Documentation](https://docs.libp2p.io/)
- [Ouroboros Consensus](../consensus/README.md)
- [Network Library](../network/README.md)
- [Building with libp2p](../docs/BUILDING_WITH_LIBP2P.md)
- [Server Integration](../SERVER_INTEGRATION.md)

## License

See [LICENSE](../LICENSE) file for details.
