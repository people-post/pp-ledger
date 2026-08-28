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
- Simple receive-process-send-close pattern (one request per TCP connection)
- Configurable request handlers
- Tiered security presets (`SecurityConfig::publicDefaults()` vs `trustedDefaults()`)
- Per-IP connection caps and connection/RPC rate limits for public-facing roles
- Bounded request queue with dedicated handler worker pool (via `Server` base)
- Read idle/total timeouts and maximum payload enforcement

**Configuration:**

`FetchServer::Config` carries `SecurityConfig` and `PerformanceConfig` (see
`FetchServerConfig.h`). Public-facing relay/miner servers use `publicDefaults()`;
trusted beacon servers use `trustedDefaults()` plus optional IP whitelist.

**Usage:**

```cpp
#include "FetchServer.h"
#include "FetchServerConfig.h"

using namespace pp::network;

FetchServer server;
FetchServer::Config config;
config.endpoint = {"127.0.0.1", 8517};
config.security = SecurityConfig::publicDefaults();
config.handler = [&server](int fd, const std::string& request, const IpEndpoint&) {
  server.addResponse(fd, "Echo: " + request);
};
server.start(config);
// ...
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

- **ppledger_lib**: Core library (Module, Logger, ResultOrError)
- **TcpClient/TcpServer**: TCP socket utilities (included in this module)

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
- **u32 big-endian length prefix** via `LedgerFrameCodec` (max payload 16 MiB on trusted paths; 512 KiB default on public paths)
- Single request-response per connection (short-lived TCP)
- Automatic connection cleanup

## Security and performance (public TCP)

Public-facing servers (relay, miner) apply `SecurityConfig::publicDefaults()`:

| Setting | Default |
|---------|---------|
| `maxPayloadBytes` | 512 KiB |
| `readIdleTimeout` | 15 s |
| `readTotalTimeout` | 30 s |
| `maxConcurrentConnections` | 4096 |
| `maxConcurrentConnectionsPerIp` | 64 |
| `maxConnectionsPerIpPerMinute` | 60 |
| `maxRpcPerIpPerMinute` | 120 |

Trusted beacon servers use `trustedDefaults()` (higher caps, rate limits disabled) and
may restrict peers via `Config::whitelist`.

`ConnectionGuard` (`platform/ConnectionGuard.h`) enforces per-IP and global caps at
accept time and tracks RPC rate after frame dispatch. `FetchServer` sweeps idle/total
read timeouts and rejects oversize length prefixes before body reads.

Performance defaults (`PerformanceConfig`):

- Handler worker pool (default 4 workers, sized by `Server` from hardware concurrency)
- Bounded request queue (4096 slots; overflow closes the client fd)
- Listen backlog 1024
- Small-response write fast path (≤ 4 KiB sent synchronously before BulkWriter)

This makes it ideal for:
- High-performance data exchange
- Blockchain data synchronization
- Peer discovery and health checks
- Simple RPC-style communication

## Future Enhancements

- Streaming support for large data transfers
- Compression support
- Encryption and authentication (TLS)
- Retry logic with exponential backoff
