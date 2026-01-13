# Multi-Node Server Architecture

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Multi-Node Network                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                           │
│  ┌─────────────┐         ┌─────────────┐         ┌─────────────┐       │
│  │   Node 1    │◄───────►│   Node 2    │◄───────►│   Node 3    │       │
│  │ (Bootstrap) │         │             │         │             │       │
│  └─────────────┘         └─────────────┘         └─────────────┘       │
│        ▲                       ▲                       ▲                │
│        │                       │                       │                │
│        │    P2P Network        │                       │                │
│        │   (libp2p/tcp)        │                       │                │
│        │                       │                       │                │
│        └───────────────────────┴───────────────────────┘                │
│                                                                           │
└─────────────────────────────────────────────────────────────────────────┘
```

## Node Internal Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                             Server Instance                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                           │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                      Application Layer                            │  │
│  │  ┌────────────┐  ┌────────────┐  ┌──────────────┐               │  │
│  │  │ Submit TX  │  │ Query State│  │ Get Peers    │               │  │
│  │  └────────────┘  └────────────┘  └──────────────┘               │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                              ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                      Server Class (pp::Server)                    │  │
│  │  ┌────────────────────────────────────────────────────────────┐  │  │
│  │  │  Consensus Thread                                           │  │  │
│  │  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │  │  │
│  │  │  │ Check Slot   │→ │ Produce Block│→ │ Broadcast    │     │  │  │
│  │  │  │ Leadership   │  │ (if leader)  │  │ to Peers     │     │  │  │
│  │  │  └──────────────┘  └──────────────┘  └──────────────┘     │  │  │
│  │  │         ↓                                      ↑            │  │  │
│  │  │  ┌──────────────┐                    ┌──────────────┐     │  │  │
│  │  │  │ Sync State   │←───────────────────│ Validate     │     │  │  │
│  │  │  │ from Peers   │                    │ Blocks       │     │  │  │
│  │  │  └──────────────┘                    └──────────────┘     │  │  │
│  │  └────────────────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                              ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    Core Components                                │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │  │
│  │  │   Ledger     │  │  Ouroboros   │  │ Transaction  │           │  │
│  │  │ (Blockchain) │  │  (Consensus) │  │    Queue     │           │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘           │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                              ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    Network Layer (when USE_LIBP2P)                │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │  │
│  │  │  libp2p      │  │ FetchClient  │  │ FetchServer  │           │  │
│  │  │   Host       │  │  (Outgoing)  │  │  (Incoming)  │           │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘           │  │
│  │         │                  │                   │                  │  │
│  └─────────┼──────────────────┼───────────────────┼─────────────────┘  │
│            │                  │                   │                     │
└────────────┼──────────────────┼───────────────────┼─────────────────────┘
             │                  │                   │
             ▼                  ▼                   ▼
    ┌────────────────────────────────────────────────────┐
    │            Network (TCP/IP, libp2p)                 │
    └────────────────────────────────────────────────────┘
```

## Consensus Flow (Multi-Node)

```
Time (Slots) ───────────────────────────────────────────────────────────►

Slot N:     Node 1 (Leader)      Node 2              Node 3
            │                    │                   │
            │ Check: Am I        │ Check: Am I       │ Check: Am I
            │ leader? YES        │ leader? NO        │ leader? NO
            │                    │                   │
            ▼                    │                   │
      ┌──────────┐              │                   │
      │ Produce  │              │                   │
      │  Block   │              │                   │
      └──────────┘              │                   │
            │                    │                   │
            │ Broadcast          │                   │
            ├───────────────────►│                   │
            │                    │                   │
            └───────────────────────────────────────►│
            │                    │                   │
            │                    ▼                   ▼
            │              ┌──────────┐        ┌──────────┐
            │              │ Validate │        │ Validate │
            │              │  Block   │        │  Block   │
            │              └──────────┘        └──────────┘
            │                    │                   │
            │                    ▼                   ▼
            │              ┌──────────┐        ┌──────────┐
            │              │   Add    │        │   Add    │
            │              │  Block   │        │  Block   │
            │              └──────────┘        └──────────┘
            │                    │                   │
            ▼                    ▼                   ▼
         [Block N]            [Block N]          [Block N]
            │                    │                   │

Slot N+1:   Node 1              Node 2 (Leader)     Node 3
            │                    │                   │
            │ Check: Am I        │ Check: Am I       │ Check: Am I
            │ leader? NO         │ leader? YES       │ leader? NO
            │                    │                   │
            │                    ▼                   │
            │              ┌──────────┐              │
            │              │ Produce  │              │
            │              │  Block   │              │
            │              └──────────┘              │
            │                    │                   │
            │    Broadcast       │                   │
            │◄───────────────────┤                   │
            │                    │                   │
            │                    └──────────────────►│
            ...                 ...                 ...
```

## Message Flow - Block Propagation

```
┌──────────┐                                               ┌──────────┐
│  Node 1  │                                               │  Node 2  │
│(Producer)│                                               │(Receiver)│
└────┬─────┘                                               └────┬─────┘
     │                                                          │
     │ 1. Create Block from Transactions                       │
     ├─────────────┐                                           │
     │             │                                           │
     │◄────────────┘                                           │
     │                                                          │
     │ 2. Validate with Consensus                              │
     ├─────────────┐                                           │
     │             │                                           │
     │◄────────────┘                                           │
     │                                                          │
     │ 3. Add to Local Ledger                                  │
     ├─────────────┐                                           │
     │             │                                           │
     │◄────────────┘                                           │
     │                                                          │
     │ 4. Serialize Block to JSON                              │
     │ {                                                        │
     │   "type": "new_block",                                  │
     │   "index": 42,                                          │
     │   "slot": 100,                                          │
     │   "hash": "0x..."                                       │
     │ }                                                        │
     │                                                          │
     │ 5. Broadcast via P2P                                    │
     │─────────────────────────────────────────────────────────►│
     │                                                          │
     │                                  6. Receive Block        │
     │                                           ┌──────────────┤
     │                                           │              │
     │                                           └─────────────►│
     │                                                          │
     │                                  7. Validate Block       │
     │                                           ┌──────────────┤
     │                                           │              │
     │                                           └─────────────►│
     │                                                          │
     │                                  8. Add to Ledger        │
     │                                           ┌──────────────┤
     │                                           │              │
     │                                           └─────────────►│
     │                                                          │
     │ 9. Acknowledge                                           │
     │◄─────────────────────────────────────────────────────────┤
     │ {"status": "received"}                                   │
     │                                                          │
```

## Message Flow - State Synchronization

```
┌──────────┐                                               ┌──────────┐
│  Node A  │                                               │  Node B  │
│ (Behind) │                                               │(Up-to-date)│
└────┬─────┘                                               └────┬─────┘
     │                                                          │
     │ Local Block Count: 50                                   │ Local Block Count: 100
     │                                                          │
     │ 1. Request Blocks                                       │
     │─────────────────────────────────────────────────────────►│
     │ {                                                        │
     │   "type": "get_blocks",                                 │
     │   "from_index": 50,                                     │
     │   "count": 100                                          │
     │ }                                                        │
     │                                                          │
     │                                  2. Fetch Blocks         │
     │                                           ┌──────────────┤
     │                                           │              │
     │                                           └─────────────►│
     │                                                          │
     │                                  3. Serialize Blocks     │
     │                                           ┌──────────────┤
     │                                           │              │
     │                                           └─────────────►│
     │                                                          │
     │ 4. Receive Blocks                                       │
     │◄─────────────────────────────────────────────────────────┤
     │ {                                                        │
     │   "type": "blocks",                                     │
     │   "blocks": [                                           │
     │     {"index": 50, ...},                                 │
     │     {"index": 51, ...},                                 │
     │     ...                                                  │
     │   ]                                                      │
     │ }                                                        │
     │                                                          │
     │ 5. Validate Each Block                                  │
     ├─────────────┐                                           │
     │             │                                           │
     │◄────────────┘                                           │
     │                                                          │
     │ 6. Add Valid Blocks                                     │
     ├─────────────┐                                           │
     │             │                                           │
     │◄────────────┘                                           │
     │                                                          │
     │ Local Block Count: 100                                  │
     │                                                          │
```

## Data Structures

### NetworkConfig
```
NetworkConfig
├─ enableP2P: bool
├─ nodeId: string
├─ bootstrapPeers: vector<string>
├─ listenAddr: string
└─ maxPeers: uint16_t
```

### Server State
```
Server
├─ Core State
│  ├─ ledger: Ledger
│  ├─ consensus: Ouroboros
│  ├─ transactionQueue: queue<string>
│  ├─ running: bool
│  └─ port: int
│
└─ P2P State (always available)
   ├─ p2pHost: shared_ptr<libp2p::Host>
   ├─ networkClient: FetchClient
   ├─ networkServer: FetchServer
   ├─ connectedPeers: set<string>
   └─ networkConfig: NetworkConfig
```

## Thread Model

```
Main Thread                 Consensus Thread              Network Threads
    │                            │                              │
    │ start()                    │                              │
    ├───────────────────────────►│                              │
    │                            │                              │
    │                            │ while(running)               │
    │                            │   Check Leadership           │
    │                            │   ┌─────────────┐            │
    │                            │   │ If Leader:  │            │
    │                            │   │  Produce    │            │
    │                            │   │  Block      │            │
    │                            │   └─────────────┘            │
    │                            │         │                    │
    │                            │         │ Broadcast          │
    │                            │         ├───────────────────►│
    │                            │         │                    │
    │                            │   ┌─────────────┐            │
    │                            │   │ Sync State  │◄───────────┤
    │                            │   └─────────────┘  Request   │
    │                            │         │          Blocks    │
    │                            │         │                    │
    │ submitTransaction()        │         │                    │
    ├───────────┐                │         │                    │
    │           │ Lock           │         │                    │
    │           │ Queue          │         │                    │
    │◄──────────┘                │         │                    │
    │                            │         │                    │
    │                            │   sleep(500ms)               │
    │                            │         │                    │
    │                            │◄────────┘                    │
    │                            │                              │
    │ stop()                     │                              │
    ├───────────────────────────►│                              │
    │                            │ Exit Loop                    │
    │ join()                     │                              │
    │◄───────────────────────────┤                              │
    │                            │                              │
```

## Compilation

The server requires libp2p:

```bash
cmake -DLIBP2P_ROOT=/path/to/libp2p-install ..
make
```

P2P networking can be enabled or disabled at runtime via `NetworkConfig::enableP2P`.

## Summary

This architecture enables:

1. **Scalability**: Nodes can join/leave network dynamically
2. **Consensus**: Distributed block production via Ouroboros
3. **Reliability**: State synchronization ensures consistency
4. **Flexibility**: P2P can be enabled or disabled via configuration
5. **Security**: Block validation at every step
6. **Performance**: Asynchronous network operations
