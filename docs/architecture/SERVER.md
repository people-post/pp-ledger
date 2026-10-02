# Server Architecture

> Canonical location: `docs/architecture/SERVER.md` (formerly `src/server/SERVER.md`).
> Doc map: [../README.md](../README.md).

This document describes the server architecture for the pp-ledger blockchain system.

## Table of Contents

1. [Overview](#overview)
2. [Components](#components)
3. [Beacon Architecture](#beacon-architecture)
4. [Relay Architecture](#relay-architecture)
5. [Miner Architecture](#miner-architecture)
6. [Network Topology](#network-topology)
7. [Configuration](#configuration)
8. [API Reference](#api-reference)
9. [Usage Examples](#usage-examples)

## Overview

The pp-ledger server architecture consists of four main components:

1. **Chain** - Base class providing common block validation and chain management
2. **Beacon** - Network validator and data archiver (extends Chain)
3. **Relay** - Trusted intermediary between beacons and miners (extends Chain)
4. **Miner** - Block producer (extends Chain)

Each component has a corresponding server wrapper (*Server classes) that handles network communication over AMP.

### Architecture Pattern

```
┌──────────────────┐
│   *Server        │  - Network communication (AMP)     
│                  │  - Request routing and JSON handling
│                  │  - Configuration loading
└────────┬─────────┘
         │ owns
         ▼
┌──────────────────┐
│  Core Logic      │  - Consensus (Beacon/Miner)
│  (Chain)         │  - Ledger management
│                  │  - Block/Transaction processing
└──────────────────┘
```

**Benefits of this composition:**

- **Separation of Concerns**: Network logic separated from business logic
- **Testability**: Core logic can be tested independently
- **Flexibility**: Multiple servers can use the same core
- **Maintainability**: Clear boundaries between components

## Components

### Chain (Base Class)

The Chain class provides common functionality for both Beacon and Miner:

**Responsibilities:**
- Block validation
- Chain management (in-memory BlockChain)
- Consensus integration (SlotCommittee)
- Ledger operations (persistent storage)
- Base configuration

**Key Methods:**
- `getBlock(blockId)` - Retrieve a block by ID
- `addBlockBase(block)` - Add a validated block to the chain
- `getCurrentBlockId()` - Get the ID of the current block
- `getCurrentSlot()` - Get the current consensus slot
- `getCurrentEpoch()` - Get the current consensus epoch

### BeaconServer + Beacon

**Purpose:** Network validator and authoritative data source

**Beacon (Core Logic) Responsibilities:**
- Maintain full blockchain history from genesis (block 0)
- Manage SlotCommittee schedule
- Track stakeholders and their stake amounts
- Determine checkpoint locations for data pruning
- Validate blocks (but does NOT produce them)
- Serve as authoritative data source for the network

**Checkpoint System:**

Beacons implement an intelligent checkpoint system to manage data growth:

- **Criteria**: Data must exceed 1GB AND blocks must be older than 1 year
- **Process**: When criteria are met, create checkpoint with essential state (balances, stakes)
- **Benefits**: Reduces storage requirements while maintaining chain integrity
- **New Node Sync**: Allows nodes to sync from checkpoint instead of genesis

**BeaconServer (Communication Layer) Responsibilities:**
- Handle network requests via AMP (`Server` request queue)
- Route requests to Beacon core logic
- Manage connections from miners and clients
- Return chain state and stakeholder information

### MinerServer + Miner

**Purpose:** Block producer

**Miner (Core Logic) Responsibilities:**
- Produce blocks when selected as slot leader
- Maintain transaction pool for pending transactions
- Sync with network to get latest blocks
- Reinitialize from checkpoints when needed
- Validate incoming blocks from other miners

**Transaction Pool:**
- FIFO queue of pending transactions
- Configurable maximum size
- Thread-safe access
- Transactions removed when included in blocks

**MinerServer (Communication Layer) Responsibilities:**
- Handle network requests via AMP (`Server` request queue)
- Automatic block production loop in background thread
- Accept transactions into pending pool
- Route requests to Miner core logic
- Provide mining status and control

### RelayServer + Relay

**Purpose:** Trusted intermediary between beacons and miners

**Relay (Core Logic) Responsibilities:**
- Sync blocks from a single upstream beacon (`syncBlocksFromBeacon`)
- Calibrate time to the upstream beacon
- Register miners just as a beacon would
- Serve chain data (blocks, accounts, transactions, status) to miners
- Participate in DHT, bootstrapped from the beacon's DHT endpoint
- Do NOT produce blocks

**RelayServer (Communication Layer) Responsibilities:**
- Expose the same request handlers as `BeaconServer` to miners: block get/add, account queries, transaction queries, status, calibration, miner registration
- Handle network requests via AMP (`Server` request queue)
- Route requests to Relay core logic

See [Relay Architecture](#relay-architecture) below for configuration details.

## Beacon Architecture

### Beacon Role in Network

Beacons serve a unique role in the pp-ledger network:

**Limited in Number:**
- Beacons are intentionally few (e.g., 5-10 globally)
- They are trusted, well-resourced nodes
- Run by network founders or elected by stakeholders

**Responsibilities:**
1. **Data Archival:** Maintain complete blockchain history
2. **Block Verification:** Validate all blocks (but don't produce them)
3. **Checkpointing:** Determine and create checkpoints
4. **Authority:** Serve as reference for chain state
5. **Coordination:** Help other nodes sync and validate

**What Beacons DON'T Do:**
- They do NOT produce blocks (that's the Miners' job)
- They do NOT participate in slot leader selection
- They do NOT mine or earn block rewards

### Beacon Configuration

`init-config.json` (genesis, `--init` only) and `config.json` (`{"port": 8517}`):
see [CONFIGURATION.md](../ops/CONFIGURATION.md).

### RPC requests

Every role speaks the same binary ledger RPC (`/pp-ledger/rpc/1.0.0` over AMP:
`binaryPack(Client::Request)` in, `Client::Response` out; types in
`src/client/Client.h`, wire in [AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md)).
There is no JSON API on the servers; pp-http offers REST on top of this RPC.

| Request | Beacon | Relay | Miner | Notes |
|---------|--------|-------|-------|-------|
| `STATUS`, `CALIBRATION` | serves | serves | serves | Chain tip, slot/epoch, network id, registry version; clock |
| `BLOCK_GET`, `ACCOUNT_GET`, `TX_GET_BY_WALLET`, `TX_GET_BY_INDEX` | serves | serves (fetches up if missing) | serves | Reads; low-priority lane |
| `BLOCK_WAIT` | serves | serves | — | Held until the tip passes the caller's; blocks propagate down |
| `BLOCK_ADD` | commits | forwards up | — | Slot leader's block; upward only, idempotent |
| `TX_ADD` | pools | forwards up | forwards up | Into the beacon's pending pool |
| `TX_PULL` | serves | forwards up | — | Slot leader pulls pending transactions |
| `REGISTER` | records | forwards up | — | Signed by the miner account's keys; renewed every 60 s |
| `MINER_LIST` | serves | forwards up | — | Registered miner ids (no addresses) |

Which origins may send what (e.g. nothing travelling up is accepted from a
node's own upstream) is one table in `Server::isAllowedFrom`; see
[LEDGER_TOPOLOGY.md §7.1](LEDGER_TOPOLOGY.md).

### Stakeholders

There is no stakeholder API: stake is account state. The stakeholders are the
non-system accounts (id ≥ 2²⁰) with a positive native-token balance, snapshotted
per epoch for leader election (WIRE_SCHEMA.md, Leader election). A new chain's
stakeholders are its genesis miners (`genesisMiners` in init-config.json); after
that, stake moves with ordinary transfers. `STATUS` reports `nStakeholders`;
`MINER_LIST` lists which stakeholders run a miner.

## Relay Architecture

### Relay Role in Network

Relay servers sit between beacons and miners:

- **Beacons** are few, trusted, and run by founders/elected stakeholders — they only communicate with trusted relays
- **Relays** sync blocks from upstream and expose the same ledger RPC to miners (opaque upstream; may chain)
- **Miners** connect via `beacons[]` — same API whether the endpoint is a relay or terminal beacon

### Relay Configuration

`config.json`: `{"beacon": "<upstream multiaddr>"}`, optional `port` (8519); the
identity key is created on first start. See [CONFIGURATION.md](../ops/CONFIGURATION.md).

### Relay requests

The relay serves the same RPC as the beacon to its downstream (table above),
answering reads from its synced chain and forwarding writes and pool requests
up to the beacon.

## Miner Architecture

### Miner Configuration

`config.json`: `{"minerId", "keys", "beacons": [<relay multiaddrs>]}`, optional
`port`, `networkAnchor`. See [CONFIGURATION.md](../ops/CONFIGURATION.md).

### Miner requests

See the RPC table above. A miner serves reads and passes `TX_ADD` up; as slot
leader it pulls transactions (`TX_PULL`) and submits its block (`BLOCK_ADD`)
to its upstream.

### Block Production Loop

MinerServer runs an automatic block production loop in a background thread:

1. Check if current slot leader
2. If yes, produce block with pending transactions
3. Broadcast block to network
4. Sleep briefly
5. Repeat

The loop runs continuously while the server is active.

### Checkpoint Reinitialization

**Problem:** New miners or miners that have been offline may have outdated ledger data.

**Solution:** Reinitialize from a recent checkpoint instead of syncing from genesis.

**Process:**
1. Load checkpoint state (balances, stakes, etc.)
2. Clear existing transaction pool
3. Rebuild ledger from checkpoint block ID
4. Prune old blocks before checkpoint
5. Resume normal operation from checkpoint

**When to Reinitialize:**

Miner is considered out of date if:
- Remote checkpoint is > 1000 blocks ahead
- Local data is corrupted or inconsistent
- Explicit reinitialization requested

## Network Topology

### Multi-Node Architecture

```
┌─────────────┐          ┌─────────────┐          ┌─────────────┐
│   Beacon    │◄────────►│   Beacon    │◄────────►│   Beacon    │
│  (8517)     │          │  (8527)     │          │  (8537)     │
└──────▲──────┘          └──────▲──────┘          └──────▲──────┘
       │                        │                        │
       │ Sync blocks             │                        │
       │                        │                        │
┌──────┴──────┐          ┌──────┴──────┐          ┌──────┴──────┐
│   Relay     │          │   Relay     │          │   Relay     │
│  (8519)     │          │  (8529)     │          │  (8539)     │
└──────▲──────┘          └──────▲──────┘          └──────▲──────┘
       │                        │                        │
       │ Beacon-compatible API  │                        │
       │                        │                        │
┌──────┴──────┐          ┌──────┴──────┐          ┌──────┴──────┐
│   Miner     │          │   Miner     │          │   Miner     │
│  (8518)     │          │  (8528)     │          │  (8538)     │
└─────────────┘          └─────────────┘          └─────────────┘
```

**Interaction Flow:**
- **Beacons** validate and archive blocks, communicate only with trusted relays
- **Relays** sync blocks from upstream and expose the same ledger RPC to miners (opaque upstream; may chain)
- **Miners** connect to relays (using the same API as they would use for a beacon directly)
- **Miners** produce blocks based on stake and submit them via the relay
- **Beacons** form a network and sync with each other
- **Beacons** create checkpoints when criteria are met (1GB + 1 year)
- **Miners** sync from checkpoints when falling behind

### Consensus Flow

```
Time (Slots) ───────────────────────────────────────────────────────────►

Slot N:     Miner 1 (Leader)     Miner 2              Miner 3
            │                    │                    │
            │ Check: Am I        │ Check: Am I        │ Check: Am I
            │ leader? YES        │ leader? NO         │ leader? NO
            │                    │                    │
            ▼                    │                    │
      ┌──────────┐              │                    │
      │ Produce  │              │                    │
      │  Block   │              │                    │
      └──────────┘              │                    │
            │                    │                    │
            │ Broadcast          │                    │
            ├───────────────────►│                    │
            │                    │                    │
            └───────────────────────────────────────►│
            │                    │                    │
            │                    ▼                    ▼
            │              ┌──────────┐         ┌──────────┐
            │              │ Validate │         │ Validate │
            │              │  Block   │         │  Block   │
            │              └──────────┘         └──────────┘
            │                    │                    │
            │                    ▼                    ▼
            │              ┌──────────┐         ┌──────────┐
            │              │   Add    │         │   Add    │
            │              │  Block   │         │  Block   │
            │              └──────────┘         └──────────┘
            │                    │                    │
            ▼                    ▼                    ▼
         [Block N]            [Block N]           [Block N]
```

## Configuration

Every field (genesis `init-config.json`, each role's `config.json`) and the
fixed network timing: [CONFIGURATION.md](../ops/CONFIGURATION.md).

## API Reference

### Error Handling

All servers return JSON error responses on failure:

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

### Thread Safety

See [THREADING.md](THREADING.md) for the model and its rollout.

- **AMP**: the `LedgerAmpRuntime` pump thread is the only thread that drives the
  AMP stack; outbound calls post to it and wait
- **All servers**: the role `runLoop` thread is the server thread. RPC requests
  are queued (`RequestQueue`, bounded) and handled one at a time on it between
  duties, so role, chain and ledger state are single-threaded

### Request limits

Requests wait in a bounded queue for the server thread; when it is full a request gets
an immediate "busy" reply, and one that waited past half the RPC timeout gets "expired".
Payloads are capped at 512 KiB. These limits are fixed
([CONFIGURATION.md](../ops/CONFIGURATION.md#network-timing-fixed)).

## Usage Examples

Setting up a beacon, relay and miners (keys, genesis miners, multiaddrs):
[SETUP.md § Multi-Node Setup](../ops/SETUP.md#multi-node-setup). Every config
field: [CONFIGURATION.md](../ops/CONFIGURATION.md).

### Testing the Setup

After starting a beacon and miner, you can test the system:

```bash
# Check beacon status
./app/pp-client -b -p 8517 current-block

# Check miner status
./app/pp-client -m -p 8518 status

# Add a transaction
./app/pp-client -b -p 8622 add-tx wallet1 wallet2 1000   # relay

# Wait for block production...
# Check if block was created
./app/pp-client -b -p 8517 current-block
```

### Stopping the Servers

Press `Ctrl+C` in the terminal where the server is running to stop it gracefully.

## Storage Management

### Work Directory Structure

```
workDir/
  ledger/
    blocks/
      00000/
      00001/
      ...
    index.dat
    checkpoints.dat
  config.json
  beacon.log (or miner.log)
```

### Ledger Persistence

- Blocks stored via `VolumeStore` (ordered volumes of `FileDirStore`)
- Automatic file rotation within a volume when files reach size limit
- Growth opens a new empty volume (`v00000N`) — never relocates old data
- Index maintained for fast block lookup

## Troubleshooting

### "Failed to start beacon"
- Ensure the work directory exists
- Ensure `config.json` exists in the work directory
- Check that the port is not already in use

### "Failed to open index file for writing"
- Ensure you have write permissions in the work directory
- The ledger subdirectory will be created automatically

### "Failed to connect to beacon"
- Ensure the beacon server is running
- Check the beacon address and port in miner's config.json
- Verify network connectivity

### Port already in use
- Change the port number in config.json
- Or stop the process using that port

## Security Considerations

1. **Beacon Trust**: Beacons must be trusted nodes (carefully selected)
2. **Private Keys**: Private keys for node identity should be secure
3. **Network Security**: Network endpoints should use TLS in production
4. **Checkpoint Integrity**: Checkpoint integrity must be cryptographically verified
5. **Stake Validation**: Verify miner has claimed stake before producing
6. **Timestamp Manipulation**: Prevent miners from manipulating block times
7. **Transaction Validation**: Verify transactions before including in blocks

## Future Enhancements

### BeaconServer
- Block broadcast to other beacons
- Chain synchronization between beacons
- Checkpoint distribution
- Miner authorization and authentication
- Distributed checkpointing coordination
- Byzantine Fault Tolerance mechanisms

### MinerServer
- Block broadcast to beacons after production
- Automatic checkpoint synchronization
- Transaction fee management
- Mining pool support
- Fee-based transaction prioritization
- Anti-spam measures for transaction pool

### Both
- TLS/SSL encryption
- Authentication and authorization
- Request logging and metrics
- WebSocket support for real-time updates
- Peer discovery mechanisms
- Advanced routing algorithms
- Network partition handling

## References

- **Design reference (Ouroboros paper; not live path)**: [Ouroboros: A Provably Secure Proof-of-Stake Blockchain Protocol](https://eprint.iacr.org/2016/889.pdf)
- **Consensus Module**: `/src/consensus/`
- **Ledger Module**: `/src/ledger/`
- **Network Module**: `/src/network/`
