# Network Module

This directory contains network communication components using cpp-libp2p for peer-to-peer communication.

## Overview

The network module provides simple fetch-style communication patterns without HTTP protocol overhead. It uses libp2p for establishing peer-to-peer connections with a simple send-receive-close pattern.

## Components

### FetchClient

A client for sending data to peers and receiving responses.

**Features:**
- Asynchronous and synchronous fetch operations
- Simple connect-send-receive-close pattern
- Error handling with ResultOrError
- Logging integration

**Usage:**

```cpp
#include "FetchClient.h"
#include <libp2p/host/host.hpp>

using namespace pp::network;

// Create libp2p host
auto host = /* create your libp2p host */;

// Create fetch client
FetchClient client(host);

// Asynchronous fetch
libp2p::peer::PeerInfo peerInfo = /* peer information */;
client.fetch(peerInfo, "/myprotocol/1.0.0", "Hello", 
    [](const auto& result) {
        if (result.isOk()) {
            std::cout << "Response: " << result.value() << std::endl;
        } else {
            std::cerr << "Error: " << result.error().message << std::endl;
        }
    });

// Synchronous fetch
auto result = client.fetchSync(peerInfo, "/myprotocol/1.0.0", "Hello");
if (result.isOk()) {
    std::cout << "Response: " << result.value() << std::endl;
}
```

### FetchServer

A server for accepting connections and handling requests.

**Features:**
- Protocol-based request routing
- Simple receive-process-send-close pattern
- Configurable request handlers
- Automatic connection management

**Usage:**

```cpp
#include "FetchServer.h"
#include <libp2p/host/host.hpp>

using namespace pp::network;

// Create libp2p host
auto host = /* create your libp2p host */;

// Create fetch server
FetchServer server(host);

// Start server with request handler
server.start("/myprotocol/1.0.0", [](const std::string& request) {
    // Process request and return response
    return "Echo: " + request;
});

// Server is now running and accepting connections

// Stop server when done
server.stop();
```

## Communication Pattern

1. **Client Side:**
   - Connect to peer
   - Send data
   - Receive response
   - Close connection

2. **Server Side:**
   - Accept connection
   - Receive data
   - Process request
   - Send response
   - Close connection

## Dependencies

- **cpp-libp2p**: Peer-to-peer networking library (installed via Hunter)
- **ppledger_lib**: Core library (Module, Logger, ResultOrError)

## Building

The network module is automatically built when building the main pp-ledger project.
Hunter package manager will automatically download and build cpp-libp2p:

```bash
cd build
cmake ..
make network
```

## Testing

Tests for the network module will be added in the `test/` directory:

```bash
./test/test_fetch_client
./test/test_fetch_server
```

## Protocol Design

The fetch protocol is intentionally simple:
- No HTTP headers or parsing overhead
- Direct binary data transfer
- Single request-response per connection
- Automatic connection cleanup

This makes it ideal for:
- High-performance data exchange
- Blockchain data synchronization
- Peer discovery and health checks
- Simple RPC-style communication

## Future Enhancements

- Connection pooling for multiple requests
- Streaming support for large data transfers
- Compression support
- Encryption and authentication
- Timeout configuration
- Retry logic with exponential backoff
