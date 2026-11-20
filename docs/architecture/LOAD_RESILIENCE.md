# Load resilience

What a node does with more load than it is built for, and what it guarantees. Two goals: under an
**overwhelming number of requests** a node degrades but never goes down, and the chain goes on; a
**burst larger than a block** is taken and included over the next blocks, not lost or bounced. The
history, measurements and the reasons for each choice: [projects/load-resilience](../../projects/load-resilience/DESIGN.md).

Related: [SERVER.md](SERVER.md) (roles, request handling), [AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md)
(the RPC protocol and its limits), [CONFIGURATION.md](../ops/CONFIGURATION.md) (the knobs, `HEALTH`),
[TEST_STRATEGY.md](../ops/TEST_STRATEGY.md) (the suites).

## Capacity comes from the chain's config

| Term | Meaning |
|------|---------|
| **Block capacity** | Transactions one block takes: `min(maxTransactionsPerBlock, what fits maxBlockBytes)`; `TX_PULL` returns up to that |
| **Designed rate** | Block capacity ÷ `slotDuration` |
| **Pool capacity** | What the beacon's `TxPool` holds: 10,000 records, 16 MiB |

With the defaults (`maxTransactionsPerBlock` 10,240, `maxBlockBytes` 4 MiB, 7 s slots) bytes bind, not
records: a transfer takes ~3.4 KB of a block (its ML-DSA-65 signature is 3,309 bytes; a block of 100
measured 341 KB), so about 1,200 fit — **~170 tx/s**. The smoke network's fixture (100 a block,
2 s slots) is 50 tx/s, small on purpose so tests fill blocks quickly. Raising the production rate is
a parameter choice: shorter slots (if a full block is sealed, relayed and committed within one),
larger blocks (4 MiB is what the RPC carries in one message), or a smaller signature scheme.

## The path of a request, and where it stops

Every layer is bounded, and refuses what it cannot hold **at once, with a code** — never by
letting the request time out.

| Layer | Limit | Past it | Set in |
|-------|-------|---------|--------|
| **Socket** | Receive / send buffers asked for: 8 MiB (`udpBufferBytes`; the OS caps them unless raised or `CAP_NET_ADMIN`). The network thread reads on without waiting while datagrams keep coming (a turn reads at most 256) | The kernel drops datagrams of every peer alike (`HEALTH` `udpReceiveDrops`) | `NodeConfig`, `LedgerAmpRuntime` |
| **Links** | 1,024 peers (`maxLinks`); 128 handshakes in progress (`handshakeBacklog`) | A full table closes its longest-idle inbound link (no request open, silent ≥ 1 s), else refuses at once; a full backlog drops the hello silently and the dialer resends it (6 times, doubling). A failed dial backs off 1 s doubling to 30 s, ±20 % | `NodeConfig`, pp-cpp-amp |
| **Requests arriving** | Small requests (≤ 64 KiB, `/pp-ledger/rpc/1.0.0`): 16 per peer arriving at once. Large (`/pp-ledger/rpc-bulk/1.0.0`, up to 4 MiB + 64 KiB): 2 per peer and 8 per node in progress (arriving or waiting for their answer); trusted peers' outside the node's 8 | One more is answered `E_RATE_LIMITED` (the peer's share) or `E_BUSY` (the node's) at once, before its body is sent. A large request from a peer outside `allowedPeers` is refused when its channel opens. A larger body on the small protocol: its channel is reset at the first fragment | `LedgerRpcProtocol.h`, `AmpLedgerServer` |
| **Request queue** | Trusted lane: 256 requests, 64 MiB of its own, served first (another lane's turn after 8). Normal: 1,024 (writes, registration). Low (reads): 256, 16 per peer, served at least every fifth of its turns with Normal. Normal and Low: 64 MiB of bodies | `E_BUSY` (retry after 1 s). A request that waited past its client's deadline, or past half the RPC timeout (7.5 s), is answered `E_EXPIRED` unserved. `HEALTH` is never queued | `RequestQueue`, `Server::enqueueRequest` |
| **Pool** (beacon) | Admission (`Chain::checkAdmission`): a submitted type, decodes, validity window open, signed, and applies through the leader's apply path on the committed state (fee floor, idempotent id, amounts, funds) — at relays too. 10,000 records / 16 MiB; 1,000 per payer | `E_POOL_FULL`, retry after ≈ the slots the backlog needs (one slot to 60 s). After each block, pooled records that no longer apply leave | `TxPool`, `BeaconServer` |
| **Forwards** (relays, miners) | To each upstream: 16 in flight (4 kept for the node's own chain work), 2 large (1 kept); 256 (16 large) waiting | One more is answered `E_BUSY` at once. A relay checks transfers (admission) and registrations (the miner's signature, for an account it holds) before forwarding them | `InFlightLimit`, `FollowerServer`, `RelayServer` |

## Answers and retries

| Code | Means | Same bytes again? |
|------|-------|-------------------|
| `E_BUSY` (10) | A queue on the way is full, the node stopping, or its upstream unreachable | Yes, after `retryAfterMs` |
| `E_RATE_LIMITED` (11) | This peer has its share in progress | Yes, after `retryAfterMs` |
| `E_POOL_FULL` (12) | The pool, or the payer's share of it, is full | Yes, after `retryAfterMs` (the pool's backlog) |
| `E_UPSTREAM_TIMEOUT` (13) | A relay's upstream did not answer in time: **it may have taken it** | Yes — the pool keeps one copy, the chain one transaction per payer and idempotent id |
| `E_EXPIRED` (14) | It waited past its client's deadline; not done | Yes |

A client retries **the same signed bytes**, never a re-signed transaction (a new signature is a new
transaction, and pays twice). `Client::submitTransaction` (pp-client `add-tx`) does: the
retry-after honoured, else backing off 0.5 s doubling to 10 s, with jitter, until the node takes it,
refuses it for good, or the transaction's validity window ends.

## Deadlines

A `Request` (version 2) carries how long its client waits (`deadlineMs`); one that waited its turn
on its client leaves with what is left. A node does not serve a request whose deadline passed in its
queue; a relay forwards with what remains less a margin (a tenth, 250 ms to 2 s), half the default
timeout (15 s) when the client sent none, and not at all below 250 ms (`E_EXPIRED`). So every hop
answers before the one before it gives up — a client learns why, not just that it timed out.

## A node's own chain work goes first

Whatever clients send, the work that makes blocks is not held up behind it:

- **On the way up**, a node's own requests (`Client::isChainDuty`: blocks, pulls for a slot leader,
  sync, `STATUS`) keep places to the upstream that forwarded client requests cannot take, and start
  first.
- **On the way in**, the peers the node's chain depends on are **trusted**: miners registered
  through it (for 10 minutes after each registration the beacon took — miners renew every minute),
  its upstreams, and peers listed in `allowedPeers`. Their requests have the trusted queue lane, and
  their large requests places of their own.

`HEALTH` `minerPeers` counts the registered miners a node trusts now.

## The chain under load

- **Renewals keep their priority** (B035): due renewals that do not fit a block wait for the next,
  and a block that leaves one out holds renewals only. Accounts written at block capacity for a while
  come due together a renewal period later and hold user transactions for **as long as the burst
  took** to write. After a re-genesis every imported account comes due at once: ⌈accounts ÷ block
  capacity⌉ blocks of renewals only ([RE_GENESIS.md](../ops/RE_GENESIS.md)).
- **Idempotency checks** read each block of a transaction's validity window from a cache of the
  newest 2,048 blocks' keys (`IdempotencyScanCache`), not from disk for every transaction: a full
  block validates in tens of milliseconds whatever the window.
- **Wallet history** (`TX_GET_BY_WALLET`) scans at most 32 blocks a request: its cost does not grow
  with the chain.

## Starting and restarting

- **A restart replays the node's ledger** (the beacon from genesis, followers from their
  checkpoint) in linear time — about as fast as a follower syncs. It is still all of it, at every
  start of a beacon.
- **Startup sync** (relays, miners) is given up only after `startupSyncTimeout` (5 min) **without a
  block authenticated or applied**: a long sync from genesis takes as long as it takes, a stuck one
  fails. A node joining a long chain starts faster from a checkpoint.

## Watching it

`HEALTH` and pp-http `/metrics` ([CONFIGURATION.md § Health and metrics](../ops/CONFIGURATION.md#health-and-metrics)):
requests served per type with queue wait and handler time; busy, refused and expired counts; slow
run-loop turns and the longest recent one; resident memory; links held; trusted miners; the socket's
receive buffer and the kernel's drops; a miner's blocks sealed, committed and lost and how late it
sealed; the beacon's pool (size, bytes, oldest age, what became of submissions).

## Measured (one machine, the smoke network)

Reference figures, to compare a run with — not limits.

| Load | What held |
|------|-----------|
| On the relay: 8,000 reads/s, 4,000 forged transfers/s, 6,000 forged registrations/s, 2,000 wallet scans/s, 400 new clients at once; reads, forged registrations or transfers and large bodies on the beacon and a miner too | Every request answered fast; `HEALTH` under 0.4 s; transfers beside taken and included; no block lost |
| 64 clients sending 4 MiB bodies at 64/s | The node takes ~50 MB/s of them (8 MiB socket buffers), refuses the rest in ~10 ms; everyone else served |
| A burst of 700 transfers from 64 new clients | Taken whole, included within 7 blocks of 100 |
| The relay's beacon frozen, then gone | Every transfer through the relay answered within its client's deadline; back within seconds of the beacon's return |
| 9 relays behind the beacon; the miners' relay restarted; each miner restarted in turn | Every relay in step; the miners registered again within 6 s; the chain went on |
| Sync from genesis; a restart's replay | ~5,300 transactions/s each (~1.6 million in 5 minutes) |

## How it is tested

Unit tests hold each limit and answer; the suites run the floods and scenarios end to end
([TEST_STRATEGY.md](../ops/TEST_STRATEGY.md)): `--suite burst` (goal 2), `--suite overload` (floods
on each role), `--suite upstream` (a slow or absent upstream), `--suite topology` (fan-out, load
into slots, churn), `--suite scale` (the data axis, in-process).
