# Quick Start Guide - Multi-Node Server

## TL;DR

The Server class supports multi-node blockchain networking using TCP sockets.

## Basic Usage

### Single Node (No Network)
```cpp
#include "Server.h"

pp::Server server(2);  // difficulty 2
server.registerStakeholder("node1", 1000);
server.setSlotDuration(5);
server.start(8080);

// Submit transactions
server.submitTransaction("tx1");
server.submitTransaction("tx2");

// Run...
std::this_thread::sleep_for(std::chrono::seconds(30));

// Check status
std::cout << "Blocks: " << server.getBlockCount() << std::endl;

server.stop();
```

### Multi-Node Network

#### Bootstrap Node (First Node)
```cpp
#include "Server.h"

pp::Server server(2);
server.registerStakeholder("bootstrap", 1000);
server.setSlotDuration(5);

pp::Server::NetworkConfig config;
config.enableP2P = true;
config.nodeId = "bootstrap";
config.listenAddr = "0.0.0.0";
config.p2pPort = 9000;

server.start(8080, config);
// Server now accepts P2P connections on port 9000
```

#### Additional Node (Joins Network)
```cpp
#include "Server.h"

pp::Server server(2);
server.registerStakeholder("node2", 1500);
server.setSlotDuration(5);

pp::Server::NetworkConfig config;
config.enableP2P = true;
config.nodeId = "node2";
config.listenAddr = "0.0.0.0";
config.p2pPort = 9001;
config.bootstrapPeers = {"192.168.1.100:9000"};  // Bootstrap node address

server.start(8081, config);
// Connects to bootstrap node and joins network
```

## Configuration Options

```cpp
struct NetworkConfig {
    bool enableP2P;                         // Enable P2P networking
    std::string nodeId;                     // Unique node identifier
    std::vector<std::string> bootstrapPeers;// Bootstrap peer addresses (host:port)
    std::string listenAddr;                 // P2P listen address
    uint16_t p2pPort;                       // P2P listen port
    uint16_t maxPeers;                      // Maximum peer connections
};
```

## API Quick Reference

### Lifecycle
```cpp
server.start(port);                     // Start single-node
server.start(port, networkConfig);      // Start with P2P
server.stop();                          // Stop server
server.isRunning();                     // Check if running
```

### Network
```cpp
server.connectToPeer(hostPort);         // Connect to peer (host:port)
server.getPeerCount();                  // Get peer count
server.getConnectedPeers();             // List peers
server.isP2PEnabled();                  // Check P2P status
```

### Consensus
```cpp
server.registerStakeholder(id, stake);  // Register for consensus
server.setSlotDuration(seconds);        // Set slot duration
server.getCurrentSlot();                // Get current slot
server.getCurrentEpoch();               // Get current epoch
```

### Transactions
```cpp
server.submitTransaction(tx);           // Submit transaction
server.getPendingTransactionCount();    // Count pending
```

### State
```cpp
server.getBlockCount();                 // Get block count
server.getBalance(walletId);            // Get wallet balance
```

## Building

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## Running Example

```bash
# Build
cd pp-ledger/build
cmake ..
make

# Run example
./server/multi_node_example
```

## Network Protocol

**Messages**: JSON format
- `get_blocks` - Request blocks from peer
- `blocks` - Response with blocks
- `new_block` - Broadcast new block

## Common Patterns

### Check P2P Status
```cpp
if (server.isP2PEnabled()) {
    std::cout << "Connected to " << server.getPeerCount() << " peers\n";
}
```

### Manual Peer Connection
```cpp
server.connectToPeer("10.0.1.5:9000");
```

### Monitor Network
```cpp
while (running) {
    std::cout << "Peers: " << server.getPeerCount() 
              << ", Blocks: " << server.getBlockCount() 
              << ", Slot: " << server.getCurrentSlot() << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
}
```

## Troubleshooting

### "Failed to initialize P2P network"
→ Check network configuration and port availability

### Peers not connecting
→ Verify firewall allows P2P port
→ Check peer addresses are correct (host:port format)
→ Ensure network connectivity

### Blocks not propagating
→ Verify nodes registered as stakeholders
→ Check slot duration is synchronized
→ Review logs for errors

## Best Practices

1. **Unique Node IDs**: Always use unique identifiers for each node
2. **Synchronized Time**: Ensure system clocks are synchronized (NTP)
3. **Firewall Rules**: Allow P2P ports in firewall
4. **Bootstrap Nodes**: Use multiple bootstrap nodes for redundancy
5. **Logging**: Enable debug logging during development
6. **Monitoring**: Track peer count and block height

## Next Steps

- See [MULTI_NODE_SUPPORT.md](MULTI_NODE_SUPPORT.md) for detailed documentation
- See [multi_node_example.cpp](multi_node_example.cpp) for complete examples
- See [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) for technical details
