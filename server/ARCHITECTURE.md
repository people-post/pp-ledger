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
│        │      (TCP)            │                       │                │
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
│  │                    Network Layer (when P2P enabled)               │  │
│  │  ┌──────────────┐  ┌──────────────┐                              │  │
│  │  │ FetchClient  │  │ FetchServer  │                              │  │
│  │  │  (Outgoing)  │  │  (Incoming)  │                              │  │
│  │  └──────────────┘  └──────────────┘                              │  │
│  │         │                   │                                     │  │
│  └─────────┼───────────────────┼────────────────────────────────────┘  │
│            │                   │                                        │
└────────────┼───────────────────┼────────────────────────────────────────┘
             │                   │
             ▼                   ▼
    ┌────────────────────────────────────────────────────┐
    │                  Network (TCP/IP)                   │
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

## Data Structures

### NetworkConfig
```
NetworkConfig
├─ enableP2P: bool
├─ nodeId: string
├─ bootstrapPeers: vector<string>  (host:port format)
├─ listenAddr: string
├─ p2pPort: uint16_t
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
└─ P2P State
   ├─ p2pServer: FetchServer
   ├─ p2pClient: FetchClient
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

```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
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
