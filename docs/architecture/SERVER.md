# Server Architecture

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

Each role has a core (its chain and what it does with it) and a server around it (the network,
the request queue, the run loop):

| Role | Core | Server |
|------|------|--------|
| Beacon: signs the tips, archives the chain | `Beacon` | `BeaconServer` |
| Relay: a full copy from genesis, served downstream | `Relay` | `RelayServer` |
| Miner: produces blocks when it leads a slot | `Miner` | `MinerServer` |

### Cores

```
NodeCore            the Chain, the certificate and equivocation stores, the reads, the rewind
├── Beacon          genesis (--init), tip signing, offers of certificates and equivocations
└── FollowerCore    opens its ledger at a starting block, adds synced blocks, checks signed tips,
    │               notes certificates and equivocations from upstreams and peers
    ├── Relay
    └── Miner       slot leadership, the block it produces, its pending transactions
```

- **`NodeCore`** owns the `Chain` (composition: `Chain` holds the ledger, consensus and
  validation). It serves the reads every role answers (blocks, accounts, transactions, authority
  proofs, the certificate and equivocation held) and the rewind a certificate may order: it writes
  the order, and applies it at the next start before the ledger mounts. A role adds to what a
  rewind does through `onRewound` (the beacon drops a tip signed above the kept blocks).
- **`FollowerCore`** is what a node following an upstream needs: `openLedger` (rewind, mount or
  create at the starting block, load, open the stores), `addBlock`, signed-tip checks, and the
  `note*` calls for certificates and evidence, which keep them if they verify.

### Servers

```
Server              the AMP listener, the request queue and its server thread, HEALTH, BLOCK_WAIT,
│                   one handler table; from the core: the chain reads, CALIBRATION, the rewind order
├── BeaconServer    --init and --promote, tip signing, the miner registry and the transaction pool
└── FollowerServer  upstreams (UpstreamSet), BlockSync and the tip watch, the clock follower,
    │               certificates and evidence up the tree, TX_ADD / EQUIVOCATION_ADD forwarded,
    │               BLOCK_GET past the tip answered after the next sync
    ├── RelayServer serves the chain downstream, forwards writes, re-serves the signed tip
    └── MinerServer registration, slot leadership, the broadcast of the block it produces
```

A role server binds its core (`bindCore`, or `bindFollower` for a follower), registers its own
handlers, and fills in the hooks where roles differ: for a follower, which upstream it may follow
(`admitUpstream`), what its first block must be (`firstBlockHash`, `predecessorDir`,
`checkStartingAuthority`), what it does with a signed tip (`onSignedTip`), after a sync
(`onSynced`) or when trust changed (`onTrustChanged`), when a sync must wait or is due
(`syncHeld`, `syncDue`), and its own work each turn of the run loop (`roleDuties`).

## Components

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
- Forward writes to the beacon (write-through): block add, transactions, miner registration;
  store a block only after the beacon accepts it
- Validate the whole chain from genesis with the same `Chain` code as the beacon
- Serve chain data (blocks, accounts, transactions, status) to miners
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
`include/ledger/client/Client.h`, wire in [AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md)).
There is no JSON API on the servers; pp-http offers REST on top of this RPC.

**Nodes never serve or link HTTP.** pp-beacon, pp-relay and pp-miner speak only this RPC; no HTTP
library (cpp-httplib, `src/lib/http`) is linked into `pp_server` or the node binaries, and the
library is built only for pp-http (`PP_LEDGER_BUILD_HTTP`). Anything an operator or a web client
wants over HTTP — the REST API, health, Prometheus `/metrics` — is pp-http talking to the nodes
through `pp::Client`; a container or host probe uses `pp-client` (e.g. `pp-client health`). A new
node capability is a new request type here plus, when HTTP is wanted, a pp-http route.

| Request | Beacon | Relay | Miner | Notes |
|---------|--------|-------|-------|-------|
| `STATUS`, `CALIBRATION` | serves | serves | serves | Chain tip, slot/epoch, network id, registry version; clock |
| `HEALTH` | serves | serves | serves | Liveness, readiness and metrics state (`Client::NodeHealth`); answered at once to any peer from the server thread's last snapshot, never queued ([CONFIGURATION.md § Health and metrics](../ops/CONFIGURATION.md#health-and-metrics)) |
| `BLOCK_GET`, `ACCOUNT_GET`, `TX_GET_BY_WALLET`, `TX_GET_BY_INDEX` | serves | serves (fetches up if missing) | serves | Reads; low-priority lane |
| `CONFIG_GET` | serves | serves | serves | The genesis meta in force at the node's tip — config, any update pending, admin keys, authority (`Chain::genesisMetaInForce`); low-priority lane. Tools build transactions and config updates from it (`pp-client config`) |
| `ACCOUNT_PROOF` | serves | serves | serves | An account (or its absence) in the tip block's state root, with its Merkle path ([WIRE_SCHEMA.md § Account proofs](../contracts/WIRE_SCHEMA.md#account-proofs)); refused while a block is being sealed; low-priority lane |
| `PRODUCER_STATS` | serves | serves | serves | Blocks of a range per producer, their fees, misses charged ([WIRE_SCHEMA.md § Producer statistics](../contracts/WIRE_SCHEMA.md#producer-statistics)); at most 1,000 blocks a reply; low-priority lane |
| `AUTHORITY_PROOF` | serves | serves | serves | Authority-changing records from genesis up to a block (beacon-authority ba5b); refused by a node that holds the chain from a checkpoint |
| `BLOCK_WAIT` | serves | serves | — | Held until the tip passes the caller's; blocks propagate down |
| `BLOCK_ADD` | commits (refused once it stood down, or above the height of equivocation evidence it holds) | forwards up; stores the block when the reply's signed tip covers it | — | Slot leader's block; upward only, idempotent; the reply carries the beacon's signed tip |
| `TX_ADD` | pools what could apply (a beacon certificate is kept first; refused once it stood down, except a certificate) | checks, then forwards up | forwards up | Into the beacon's pending pool; a beacon certificate is also kept by each node it passes (`CertificateStore`, `data/beacon-cert.dat`) |
| `TX_PULL` | serves | forwards up | — | Slot leader pulls pending transactions, oldest first, up to a block's records and bytes (`maxTransactionsPerBlock`, `maxBlockBytes`) |
| `REGISTER` | records | checks the signature (an account it holds), then forwards up | — | Signed by the miner account's keys; renewed every 60 s. The node it was taken through trusts the sender's peer for 10 minutes (§ Request limits) |
| `MINER_LIST` | serves | forwards up | — | Registered miner ids (no addresses) |
| `EQUIVOCATION_ADD` | keeps it and halts | keeps it, forwards up | keeps it, forwards up | Evidence that the beacon key signed two blocks at one height ([BEACON_AUTHORITY.md § Equivocation halt](../contracts/BEACON_AUTHORITY.md#equivocation-halt)); `data/equivocation.dat`; upward only |

**The beacon's pool** (`TxPool`) holds only what could apply (load-resilience lr4). A `TX_ADD` is
checked first on the committed state (`Chain::checkAdmission`): it is a type that is submitted at
all (not a genesis, renewal, account end or import, which the chain writes), its validity window is
open, its signer's keys signed it, and it applies through the leader's own apply path on a scratch
state — the fee floor, a non-zero idempotent id not used yet, amounts in range, funds, the type's
rules. What depends on the rest of a block (pending spends, ids pending in it) is the leader's. Relays
run the same check before forwarding; miners, which may hold the chain from a checkpoint, pass it
on. A payer may have at most `maxPerSender` (1,000) records pooled (the admins' records without an
id count against the genesis account); the pool holds 10,000 records
or 16 MiB, each for its validity window or 10 minutes. After each block it commits, the beacon drops
pooled records whose idempotent id the block used or that no longer apply (the payer spent the
funds, the window ended), so no leader keeps pulling and refusing them.

Behaviour under load — every limit on a request's path, the answers and how clients retry,
deadlines, a node's own chain work going first — is in [LOAD_RESILIENCE.md](LOAD_RESILIENCE.md); in
short:

**Overload is answered, not left to time out** (load-resilience LR006, LR007): a full queue
answers `E_BUSY`, a full pool (or payer's share) `E_POOL_FULL`, each with when to try again; a request
whose client's deadline passed in the queue is not served (`E_EXPIRED`); a request that waited its
turn on its client leaves with what is left of its deadline; a relay forwards with what
remains of the client's deadline and passes the upstream's answer on — `E_UPSTREAM_TIMEOUT` when the
upstream did not answer in time (it may have taken the request). `Client::submitTransaction` (used
by pp-client) sends the same signed bytes again after any of these, never a re-signed transaction:
the pool keeps one copy, the chain one transaction per payer and idempotent id.

**A node's own chain work goes first** (load-resilience LR009). On the way up, what a node sends for
itself — blocks, pulls for a slot leader, sync, `STATUS` — keeps 4 of its 16 places to the peer
(1 of 2 for large requests) that requests forwarded for clients cannot take, and starts before them;
at most 256 forwarded requests wait for a peer, and one more is answered `E_BUSY` at once
(`InFlightLimit`). On the way in, the peers a node's chain depends on — miners registered through it,
its upstreams, peers listed in `allowedPeers` — are trusted: their requests have their own queue lane,
served first, and their large requests places outside the node's share. A flood of client requests,
forged registrations or reads cannot make a relay's miners miss their slots.

Which origins may send what (e.g. nothing travelling up is accepted from the
upstream a node follows) is one table in `Server::isAllowedFrom`; see
[LEDGER_TOPOLOGY.md — Request direction](LEDGER_TOPOLOGY.md#request-direction).

Each role registers its handlers in one table (`Server::handle` for a reply now, `handleLater` for
one that waits on another server). The base registers the chain reads and `CALIBRATION` for every
role from its core (`bindCore`); a role replaces one where it differs (a follower fetches a block
past its tip; a syncing miner refuses `CALIBRATION`). `Server::admit` may refuse a request before
its handler (the beacon refuses producer writes once it stood down or halted). A type with no
handler gets "Unsupported request type".

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
- **Every node** rotates checkpoints deterministically once `checkpointMinBlocks` /
  `checkpointMinAgeSeconds` are met
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

- **AMP**: one thread drives the AMP stack — the `LedgerAmpRuntime` pump, or on a
  host (`Server::runOnHost`) the host's driver; outbound calls post to it and wait
- **All servers**: the role `runLoop` thread is the server thread. RPC requests
  are queued (`RequestQueue`, bounded) and handled one at a time on it between
  duties, so role, chain and ledger state are single-threaded

### Request limits

Requests wait in a bounded queue for the server thread (by count, and 64 MiB of bodies); when it
is full a request gets an immediate "busy" reply, and one that waited past half the RPC timeout
gets "expired". Three lanes (`RequestQueue`): Trusted (the peers above: 256 requests, its own
64 MiB, served first but giving another lane a turn after 8), Normal (writes, registration,
completions: 1,024) and Low (reads: a quarter of that, 16 per peer, served at least every fifth
pop). Messages are sized from the chain's block limit: a block or a record (`BLOCK_ADD`,
`TX_ADD`) up to 4 MiB + 64 KiB, any other request 64 KiB; a reply too large to send is an error
reply ([AMP_TRANSPORT.md § Protocol](../contracts/AMP_TRANSPORT.md#protocol)). These limits are fixed
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

# Register an account as a miner (signed with its key); elected from the next epoch
./app/pp-client -b -p 8622 miner-register <account> -f <fee> -k <key>
./app/pp-client -b -p 8622 miner-revoke <account> -f <fee> -k <key>

# Wait for block production...
# Check if block was created
./app/pp-client -b -p 8517 current-block
```

### Stopping the Servers

`Ctrl+C` (SIGINT) or SIGTERM — what `docker stop` and systemd send — stops a server gracefully,
exit code 0; `pp-http` too. A stop during startup (a relay or miner syncing from its upstream for
up to 5 minutes, or a miner waiting that long for an upstream to answer — its relay may be
restarting too) ends the start at once: the node does not go on to run.

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

1. **Beacon authority**: nodes verify the beacon's signed tips back to genesis
   ([BEACON_AUTHORITY.md](../contracts/BEACON_AUTHORITY.md)). Keep the beacon signing key on the
   beacon host only, the admin (genesis) keys offline, and the recovery keys apart from both
2. **Private Keys**: Private keys for node identity should be secure
3. **Network Security**: Network endpoints should use TLS in production
4. **Checkpoint Integrity**: a miner pinning genesis checks its starting checkpoint against
   `AUTHORITY_PROOF`; an unpinned node takes its first block on trust
5. **Stake Validation**: Verify miner has claimed stake before producing
6. **Timestamp Manipulation**: Prevent miners from manipulating block times
7. **Transaction Validation**: Verify transactions before including in blocks

## References

- **Design reference (Ouroboros paper; not live path)**: [Ouroboros: A Provably Secure Proof-of-Stake Blockchain Protocol](https://eprint.iacr.org/2016/889.pdf)
- **Consensus Module**: `/src/consensus/`
- **Ledger Module**: `/src/ledger/`
- **Network Module**: `/src/network/`
