# Load resilience — design

**Status:** lr0–lr2 done (design, metrics, load generator); first measurements in § Measured
**Decisions:** [DECISIONS.md](DECISIONS.md) · **Status today:** [CURRENT_STATE.md](CURRENT_STATE.md) ·
**Order of work:** [PHASES.md](PHASES.md)

What the network does under more load than it is built for, and how we prove it. Two goals:

1. **Overload: survive.** An overwhelming number of requests, at one node or across the network,
   degrades service but does not take a node down or stop the chain.
2. **Bursts: absorb.** With the average rate below the designed rate, a burst of transactions
   larger than one block is taken in and included over the next blocks, not lost or bounced.

---

## Capacity comes from the chain's config

No absolute numbers: every target is derived from the config in force, so the same tests hold for
any network.

| Term | Meaning |
|------|---------|
| **Block capacity** | Transactions one block takes: `min(maxTransactionsPerBlock, what fits maxBlockBytes, TX_PULL_MAX_RECORDS)` |
| **Designed rate** | Block capacity ÷ `slotDuration` |
| **Burst** | More transactions submitted at once than one block's capacity |
| **Pool capacity** | What the beacon's `TxPool` holds (`Limits`: records, bytes) |

With the defaults (10,000 records, 4 MiB, a pull of 1,000, 7 s slots) the pull is the binding
limit: about 1,000 per block, ~140 tx/s.

---

## Three axes of load

Load grows along three independent axes; each goal is tested along each of them.

| Axis | What grows | What breaks first (expected) |
|------|------------|------------------------------|
| **Requests** | Requests per second at one node | The server thread (one per node: requests are served between its duties), the queues, the network thread on new links |
| **Data** | Accounts, chain length, block size | Request cost (wallet history scans the chain backwards on the server thread); startup and sync from genesis against `startupSyncTimeout` (5 min); the renewal storm a re-genesis schedules (imported accounts all come due one renewal period after the import, [RE_GENESIS.md](../../docs/ops/RE_GENESIS.md)) |
| **Topology** | Relays, miners, hops, link quality, churn | What only appears between nodes (below) |

### Why one server does not cover the topology axis

1. **Timeouts stack across hops.** A miner's `TX_ADD` goes to a relay, which forwards it to the
   beacon: every hop has the same 15 s RPC timeout, and a queued request expires after half of it.
   Under load the relay's upstream call and its client's call can time out together — each server
   healthy alone, the chain of them failing.
2. **Fan-out and herds.** Each new block wakes every parked `BLOCK_WAIT` (as many as the request
   queue holds), then the syncs and `BLOCK_GET`s follow against the relay; every node probes each
   upstream with `STATUS` every minute; a miner broadcasts each block (up to 4 MiB) to every
   upstream. The beacon's load grows with the relays and miners behind it.
3. **Latency becomes consensus.** A leader must pull transactions, seal, and get the beacon's
   signed tip back through a relay within the delivery window. Slow under load, it misses the slot;
   missed slots deregister miners (`minerMaxMissedSlots`), which changes the committee.
4. **Storms.** A rewind restarts every node at once; a new certificate makes every node re-rank
   and push it up; a relay restart makes all its miners re-register and resync together.
5. **The network itself.** Loss and latency on a WAN link (relay to beacon) change retransmits and
   sync timeouts; covered in-process today ([TESTING.md](../../docs/architecture/TESTING.md)
   injectors), Docker netem planned.

---

## Goal 1 — overload: what "survive" means

| Check | Pass |
|-------|------|
| Alive and bounded | No crash; memory under a ceiling; every queue bounded |
| Its own duties go on | The beacon commits and signs every block it gets; slot leaders seal in their slots (no miner deregistered for missed slots because of the load); relays stay within the readiness lag of the tip |
| Overload is answered fast | Excess requests get "busy" within milliseconds, not a timeout; `HEALTH` answers |
| It recovers | Back to normal within a bound once the load stops, without a restart |

Loads, each against one node and then through relays: cheap reads; expensive reads (wallet
history scans the chain backwards, on the server thread); `TX_ADD` and `REGISTER`; parked
`BLOCK_WAIT`s; oversized requests; many new peer identities (each new link costs an ML-KEM /
ML-DSA handshake on the network thread — a cost the request queue never sees).

## Goal 2 — bursts: what "absorb" means

With the average below the designed rate, a burst of *B* valid transactions, *B* above block
capacity and up to pool capacity:

- every one is included within ⌈*B* ÷ block capacity⌉ blocks plus a small slack, oldest first;
- none is lost silently: a submit is either taken (and then included or expired with a reason the
  client can see) or answered "full, retry";
- "retry" means the pool is full — not that a queue on the way was momentarily busy.

---

## Where we start

From the code today (develop after #133). These are the expected findings; lr3 measures them.

| Today | Consequence |
|-------|-------------|
| `TxPool` admits anything that decodes: no signature, fee or account check | Junk takes pool capacity from valid transactions |
| A leader rejects an invalid pulled transaction for its slot only; the pool drops a transaction only when a block includes it or it expires (`ttlSeconds` 600) | Invalid transactions at the head are pulled again every slot: 1,000 of them hide every valid one behind them for up to ten minutes |
| `TX_PULL_MAX_RECORDS` (1,000) is below `maxTransactionsPerBlock` (10,000) | The pull, a constant in code, sets the drain rate, not the chain's config |
| The pool is first come, first served, with no share per sender | One account (or one relay's clients) can fill it |
| A relay forwards each `TX_ADD` as its own upstream request | A burst meets the beacon's per-peer and queue limits (16 arriving per peer, 1,024 queued, 64 MiB) and is bounced "busy" while the pool has room |
| One server thread serves requests between the node's duties | A slow handler (a long wallet scan) delays commits, seals and syncs |
| RPC timeout 15 s on every hop; a queued request expires after half of it | Forwarded requests can time out on two hops at once |

### Measured: first runs (lr2)

Smoke network (beacon, relay, 3 miners; 2 s slots, 100 records a block), load through the relay with
`pp-client load` ([TEST_STRATEGY.md § Load generator](../../docs/ops/TEST_STRATEGY.md#load-generator)).

| # | Run | Result |
|---|-----|--------|
| M1 | 20/s for 15 s, 10 % junk, 4 identities | Every valid transfer included within 2 blocks (p99 3.1 s); replies p50 20 ms; junk pooled, never included |
| M2 | 200 accounts created with the fixture's renewal every 2 blocks | 8 never created: once more accounts are due than a block holds, every block is renewals-only (B035) and user transactions wait for good |
| M3 | Burst of 500 at 2 s + 10/s, 4 identities (~125 at once each) | 446 refused at once by the relay's transport ("channel not open": 16 requests in flight per peer); 254 taken and included |
| M4 | Same, an earlier run | 374 replies timed out at the client's 30 s and 90 were refused, yet ~200 of them were pooled and included: the client is told it failed, the transfer goes through (a retry signed anew pays twice) |
| M5 | Same burst from 64 identities | 364 "dial timeout" (8 s), 72 "dial in backoff" (30 s), 85 "channel not open": 65 new clients at once against a node that takes 48 links and 16 handshakes at a time (Amp `max_links`, `max_pending_inbound`) |
| M6 | Relay and beacon during all of these | Never busy, no request expired, no run-loop turn over 140 ms, ready throughout, back to normal at once |

So: the servers survive (goal 1 held so far, M6), but bursts are not absorbed (goal 2): they are
bounced at the first hop (M3, M5), half-delivered with wrong answers (M4), or starved by renewals
(M2). Added to the table above:

| Today | Consequence |
|-------|-------------|
| A node takes 48 links, 16 handshaking (Amp defaults; the ledger sets none) | Clients past ~45 per relay are refused slowly (an 8 s dial timeout, then 30 s of backoff) |
| Over the per-peer limit, a request is refused by the transport | The client sees "channel not open", not "busy, retry" |
| Due renewals take a block before any user transaction (B035), and come due together (all accounts written in one burst renew one period later, together) | A burst of account creation comes back as a renewal burst that holds every user transaction for ⌈burst ÷ block capacity⌉ blocks; with more accounts due per period than blocks can renew, for good |
| The client's timeout (30 s for TX_ADD) equals the relay's forward timeout | Both expire together; whether the beacon pooled it is unknown to the client |

What already holds: bounded request queue with lanes (reads cannot crowd out writes), per-peer
caps, "busy" before queueing, expiry of requests their client gave up on, `HEALTH` answered off the
queue, block and request byte limits (B034).

---

## How we test

Cheapest tier that answers the question ([TESTING.md](../../docs/architecture/TESTING.md)):

| Tier | What it owns here |
|------|-------------------|
| Unit | Pool admission, eviction, fairness, drain order; queue and lane limits; "busy" paths |
| Single server | The requests axis: one role under a load generator, relays and miners against a scripted fake upstream (slow, refusing, absent): capacity curves, goal 1's checks per role |
| Ledger scale | The data axis: large generated ledgers (many accounts, long chains); request cost, startup and sync from genesis within `startupSyncTimeout`, the renewal storm after a re-genesis |
| Network | The topology axis, only what one server cannot show (§ Why one server does not cover the topology axis): a fan-out ladder (relays × miners) measuring the beacon's fan-in; steady transactions end to end; timeouts across hops; load turning into missed slots; churn — rolling restarts, a relay crashing while its miners lead, a rewind or a new certificate across the fleet; optional loss and latency (netem) |

Tools:

- **Load generator** (`pp-client load`): open loop — a fixed rate whatever the replies, so a slow
  server shows as latency, not as a slower client; a mix of request types; many peer identities;
  valid transactions signed by many funded accounts, and invalid ones on demand; reports latency
  percentiles, every outcome by kind, and inclusion latency for transactions.
- **Funded-accounts fixture**: a genesis (init-config) with N funded accounts and their keys.
- **Metrics the checks read** (pp-http `/metrics`, `HEALTH`): request latency and queue wait per
  type, "busy" and expired counts, duty lateness (slot end → seal → commit), pool size and age.
  For the network: slot hit rate (leader slots sealed and committed), transaction inclusion
  latency (p99 in slots), relay lag, memory per node.

Small "fails cleanly" cases gate PRs; capacity curves and network scenarios run nightly or by hand.

---

## Open questions

- Pool admission: which checks are cheap enough on the beacon's server thread (an ML-DSA verify
  per `TX_ADD`), and which belong at relays first?
- Permanently invalid vs. invalid for now (a balance that a pending transaction will fund): what
  may the leader report back so the pool drops a transaction?
- Per-sender share: by account, by submitting peer, or both; what happens to a sender over its share.
- Relay forwarding: batch (a new request type) or a relay-side queue drained at a bounded rate.
- The pull limit: derive it from the config (records and bytes) instead of a constant.
- Renewals (M2): may user transactions take a share of a block while due renewals wait (a
  consensus rule, B035), and may renewals spread over the period instead of all coming due at once?
- Links (M5): how many clients a relay should hold (`max_links`, `max_pending_inbound` for the
  ledger), and whether a full table should refuse at once instead of letting the dial time out.
- Answers under overload (M3, M4): "busy, retry" instead of a transport error past the per-peer
  limit; a forward timeout shorter than the client's, so the relay can say what happened.
