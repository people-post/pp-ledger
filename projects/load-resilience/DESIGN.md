# Load resilience — design

**Status:** lr0–lr7 done: goal 2 (bursts) met on the smoke network (`--suite burst`); goal 1 (overload) next (lr8–lr10); measurements in § Measured
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
| M7 | M3's burst after lr4–lr6 (clients queue past 16 in flight, deadlines forwarded, overload codes) | 700 of 700 taken and included, within 7 blocks (p50 4); replies p99 0.76 s |
| M8 | M5's burst from 64 identities after lr6b (TCP-like links; `maxLinks` 1,024, `handshakeBacklog` 128) | 700 of 700 taken and included, within 7 blocks; replies p99 0.81 s |
| M6 | Relay and beacon during all of these | Never busy, no request expired, no run-loop turn over 140 ms, ready throughout, back to normal at once |

| M9 | Relay: 2,000 then 8,000 `STATUS`/s from 64 identities, 10 tx/s beside (lr8) | All served (p99 15 ms, then 200 ms); never busy; no run-loop turn over 101 ms; the transfers included within 2 blocks, no block abandoned |
| M10 | Relay: 4,000 junk transfers/s (each signature checked on the server thread), 10 tx/s beside | All refused (p99 560 ms); never busy; the transfers unaffected |
| M11 | Relay: 400 new identities at once, 800 `STATUS`/s, 10 tx/s beside | All served (p99 46 ms); resident memory 21 → 24 MB: ~7 KB per idle link |
| M12 | Relay: 64 identities sending ~4 MiB `BLOCK_ADD` bodies at 64/s (256 MiB/s), 10 tx/s beside | Memory bounded (64 MB); run loop never slow; but the bodies mostly never arrived (channel not open, timeouts, dial timeouts), a `HEALTH` probe went unanswered for 5 s, and every other peer suffered — the transfers beside: replies p50 0.94 s (from 20 ms), p99 at the 30 s timeout, 11 `E_BUSY` because the relay's own channel to the beacon could not open, inclusion 12 s instead of 2 s |
| M13 | Start-up, no load | The first blocks after the network starts (slots 2–7) were refused by the beacon as late ("after its delivery deadline") — every miner in turn; from then on none. To look at in lr10 |
| M14 | M12 again after LR008 (large requests on their own protocol, 2 per peer and 8 per node in progress, refused at once), on this host's 208 KiB buffer cap | The flood: 31 bodies received, 481 answered `E_BUSY` at once (p50 6 ms). `HEALTH` answered every time (20 probes, max 0.5 s). The transfers beside: 200 of 200 taken and included, replies p50 25 ms, p99 0.31 s, none busy. Kernel drops at the relay 1.09 M → 126 k: eight 4 MiB bodies at once still overrun 208 KiB |
| M15 | M14 with the 8 MiB buffers granted (`CAP_NET_ADMIN`, in a container) | First try worse than M14 (transfers p50 0.88 s, probes up to 1.3 s, 172 k drops): a bigger buffer only queued more. Two causes: frames a refused client had queued were still sent after the relay closed the channel (Amp); and the relay's network thread, at 20–40 % CPU, slept 5 ms after each turn of 256 datagrams — a ceiling of ~50 k datagrams/s whatever its CPU. Fixed both; then the request queue (64 MiB) filled with received bodies and small requests got `E_BUSY` (58 of 200), so a large request now holds its place until answered. Final: **no drop**; probes max 69 ms; 107 bodies taken (~50 MB/s, 4× M14), the rest refused in 11 ms; transfers 200 of 200, p50 35 ms, p99 1.07 s — the relay's server thread decoding 107 junk blocks of 4 MiB |

From M9–M12: a node takes a flood of requests well — one server thread, queues and lanes hold — but
not a flood of **bytes**: one network thread carries every peer's packets, and a few peers sending
large bodies starve everyone else, handshakes, health probes and the node's own upstream link
included. Memory per idle link is small (M11); what bounds memory is receiving (16 requests of up to
4 MiB per peer, no node-wide cap).

| M16 | Relay: forged miner registrations (a funded account's id, a signature of the right size) at 2,000/s, then 6,000/s, 10 tx/s beside | At 2,000/s all forwarded, refused by the beacon; nothing else touched. At 6,000/s 7,027 got no answer before the client's 15 s timeout, and a miner's block reached the beacon after its delivery deadline: the relay's 16 places to the beacon were full of forwarded registrations. After LR009: all 48,000 refused at the relay (p99 0.31 s), nothing reached the beacon, the transfers beside p99 0.11 s, no block lost |
| M17 | Relay: valid transfers at 2,000/s (past the pool) | Every one answered fast (p99 0.46 s): 5,317 taken, 9,652 "pool full", 1,031 "busy" (the relay's queue to the beacon full, LR009); no block lost; the pool's backlog took ~54 blocks to drain |
| M18 | Relay: wallet-history scans at 2,000/s | The low lane answered "busy" at once (p99 0.24 s), the transfers beside untouched — but a miner's own sync (`BLOCK_GET`, `BLOCK_WAIT`, in the same lane) got "busy" too: it fell behind and sealed block 24 nine times over a stale tip ("conflicts with the stored block"). After LR009 (its own trusted lane): no block lost |
| M19 | A herd of 400 new clients right after other floods (one host: the relay, beacon, miners and 400 client runtimes) | ~0.5 % of the dials gave up after 8 s (the handshake backlog drops hellos, TCP-like, and a few exhaust their resends); alone, none. `pp-client load` now counts such requests `unsent`, not `timeout` |
| M20 | Full blocks (100 transfers of ~3.4 KB) during M17 | A leader sealed past its slot's delivery deadline: each transaction's idempotency check re-read and decoded every block of its validity window (here 60 s: ~30 full blocks), so validating a block of 100 took 140–290 ms on a short chain and grew with it. With the blocks' keys kept (LR010): 33–37 ms |

After LR008 (M14, M15) the flood is confined to large requests: everyone else is served, slower only
by the server thread's time on the large ones, and the large requests themselves are told to come
back (`E_BUSY`) instead of timing out. What a large request costs the server thread (decoding a
block) is the next limit — a per-role question for the rest of lr8.

So: the servers survive (goal 1 held so far, M6), but bursts are not absorbed (goal 2): they are
bounced at the first hop (M3, M5), half-delivered with wrong answers (M4), or starved by renewals
(M2). Added to the table above:

| Today | Consequence |
|-------|-------------|
| A node takes 48 links, 16 handshaking (Amp defaults; the ledger sets none) | Clients past ~45 per relay are refused slowly (an 8 s dial timeout, then 30 s of backoff) |
| Over the per-peer limit, a request is refused by the transport | The client sees "channel not open", not "busy, retry" |
| Due renewals take a block before any user transaction (B035), and come due together (all accounts written in one burst renew one period later, together) | A burst of account creation comes back as a renewal burst that holds every user transaction for ⌈burst ÷ block capacity⌉ blocks; with more accounts due per period than blocks can renew, for good |
| The client's timeout (30 s for TX_ADD) equals the relay's forward timeout | Both expire together; whether the beacon pooled it is unknown to the client |
| A leader checks a transaction's idempotent id by reading every block of its validity window (`checkIdempotency`) | Each transaction a leader takes costs reads of up to its window's blocks (megabytes); not used for admission for that reason — for lr8 / lr9 |

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
- The pull limit: derive it from the config (records and bytes) instead of a constant.
- Decided: blocks under a flood of large requests — miners registered through a relay are trusted,
  with large-request places of their own (LR009).
- Decided: renewals keep B035, the echo is measured ([LR004](DECISIONS.md#lr004--due-renewals-keep-their-priority-b035-the-echo-is-measured-not-changed));
  links like TCP ([LR005](DECISIONS.md#lr005--links-behave-like-tcp-many-connections-a-handshake-backlog-silent-drop-fast-refusal));
  deadlines carried hop by hop ([LR006](DECISIONS.md#lr006--requests-carry-the-clients-deadline-each-hop-forwards-what-remains));
  overload answered in codes, clients retry the same bytes ([LR007](DECISIONS.md#lr007--overload-is-answered-in-codes-with-a-retry-after-clients-retry-the-same-bytes));
  large requests have their own share of a node ([LR008](DECISIONS.md#lr008--large-requests-have-their-own-protocol-and-a-node-wide-share-the-socket-is-sized-for-them));
  a node's own chain work goes first ([LR009](DECISIONS.md#lr009--a-nodes-own-chain-work-goes-first-on-the-way-up-and-on-the-way-in));
  idempotency checks read each block once ([LR010](DECISIONS.md#lr010--idempotency-checks-read-each-block-once)).
