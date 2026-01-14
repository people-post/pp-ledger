# Network Module

This directory contains network communication components using TCP sockets for peer-to-peer communication.

## Overview

The network module provides simple fetch-style communication patterns without HTTP protocol overhead. It uses TCP sockets for establishing peer-to-peer connections with a simple send-receive-close pattern.

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

using namespace pp::network;

// Create fetch client
FetchClient client;

// Asynchronous fetch
client.fetch("127.0.0.1", 8888, "Hello", 
    [](const auto& result) {
        if (result.isOk()) {
            std::cout << "Response: " << result.value() << std::endl;
        } else {
            std::cerr << "Error: " << result.error().message << std::endl;
        }
    });

// Synchronous fetch
auto result = client.fetchSync("127.0.0.1", 8888, "Hello");
if (result.isOk()) {
    std::cout << "Response: " << result.value() << std::endl;
}
```

### FetchServer

A server for accepting connections and handling requests.

**Features:**
- Port-based listening
- Simple receive-process-send-close pattern
- Configurable request handlers
- Automatic connection management

**Usage:**

```cpp
#include "FetchServer.h"

using namespace pp::network;

// Create fetch server
FetchServer server;

// Start server with request handler
server.start(8888, [](const std::string& request) {
    // Process request and return response
    return "Echo: " + request;
});

// Server is now running and accepting connections

// Stop server when done
server.stop();
```

## Communication Pattern

1. **Client Side:**
   - Connect to host:port
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

- **ppledger_lib**: Core library (TcpClient, TcpServer, Module, Logger, ResultOrError)

## Building

The network module is automatically built when building the main pp-ledger project:

```bash
mkdir build && cd build
cmake ..
make network
```

## Testing

Tests for the network module are in the `test/` directory:

```bash
cd build
ctest -R test_fetch --output-on-failure
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
- Encryption and authentication (TLS)
- Timeout configuration
- Retry logic with exponential backoff
