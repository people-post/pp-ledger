# Load resilience — decisions

Rationale for [DESIGN.md](DESIGN.md). Outcomes move to `docs/` when they ship; mark the entry
**superseded by** that doc then.

## LR001 — Two goals: survive overload, absorb bursts

**Date:** 2026-10-17
**Decision:** Load work serves two goals. Overload: an overwhelming number of requests degrades
service but takes no node down and does not stop the chain. Bursts: with the average below the
designed rate, a burst larger than one block is included over the next blocks, not lost or bounced.
**Rationale:** These are the loads the network must meet in practice — a flood (abuse, a
misbehaving client, a herd after a restart) and an uneven but sustainable stream of transactions.
**Alternatives:** A target throughput figure (depends on config and hardware; says nothing about
behaviour past it).

## LR002 — Targets are derived from the chain's config

**Date:** 2026-10-17
**Decision:** Block capacity, the designed rate and pool capacity come from the config in force
(DESIGN § Capacity); tests state their loads as multiples of them.
**Rationale:** The same tests then hold for any network and any config change; a config change
that lowers capacity shows up as a different load, not a broken test.
**Alternatives:** Absolute numbers per environment (to be re-tuned with every config change).

## LR003 — Three axes; single-server tests carry most of the work, the network only what needs it

**Date:** 2026-10-17
**Decision:** Load is tested along three axes: requests, data and topology (DESIGN § Three axes).
Each role is loaded alone, followers against a scripted upstream; that tier owns the requests axis,
capacity and overload behaviour. Generated large ledgers own the data axis. Network scenarios own
the topology axis and cover only what one server cannot show: timeouts across hops, fan-out, load
turning into missed slots, storms, link quality.
**Rationale:** One server is cheaper, deterministic enough to gate on, and points at the cause;
multi-node runs are expensive and flaky, and most of their failures reduce to one server's
([TESTING.md](../../docs/architecture/TESTING.md): cheapest tier that answers).
Data is its own axis: a large ledger slows a node with no extra request or node at all.
**Alternatives:** Network-level load only (realistic, but slow, flaky and hard to localise).
Single-server only (misses emergent behaviour across hops, and big ledgers).

## LR004 — Due renewals keep their priority (B035); the echo is measured, not changed

**Date:** 2026-10-17
**Decision:** No change to B035 now: due renewals that do not fit wait for the next block, and a
block that leaves one out holds renewals only. lr9 measures the echo — accounts written at block
capacity for a while come due together a renewal period later and hold user transactions for about
as long (longer when slots go empty) — and the load fixture uses a realistic renewal period.
**Rationale:** With a one-year period, renewals per block follow the writes per block a year
earlier, which a block could hold; only busy periods echo, and for about their own length. A share
for user transactions while renewals wait is a consensus change, worth it only if the measured
stall is not acceptable. The smoke fixture's renewal every two blocks is what starved transactions
for good (DESIGN § Measured, M2).
**Alternatives:** A share of each block for user transactions while renewals wait (consensus
change; postponed). Spreading renewals over the period (changes when accounts are due; postponed).

## LR005 — Links behave like TCP: many connections, a handshake backlog, silent drop, fast refusal

**Date:** 2026-10-17
**Decision:** Amp's link table and handshake quota become configurable and the ledger sets them per
role (`maxLinks`, `handshakeBacklog`), with defaults sized from measured memory per link (lr8),
large enough for thousands of clients on a relay or beacon. Like a full listen backlog, a
handshake past the backlog is dropped silently and the dialer resends its handshake on a backoff
within its dial timeout (SYN retries); like a closed port, a node whose table is full of active
links refuses at once with a packet no larger than the probe (no amplification) — after first
evicting its longest-idle inbound link. The dial-failure backoff grows and is jittered.
**Rationale:** 48 links and 16 handshakes refuse the 46th client of a relay, and slowly: an 8 s dial
timeout, then 30 s of backoff, in step for a herd (M5). TCP's split has served servers for decades:
connections bounded by memory, a backlog bounding half-open work (here: ML-KEM / ML-DSA CPU), silent
drops that let a herd in over a few seconds, an immediate answer when there is nothing to wait for.
**Alternatives:** Raise the constants only (a herd still fails in step, a full table still times
out). Refuse every overflow at once (a herd retries in step; a forged-source flood gets answers).

## LR006 — Requests carry the client's deadline; each hop forwards what remains

**Date:** 2026-10-17
**Decision:** A request carries the time its client will wait. Each hop forwards with what remains
less a margin, a queued request whose deadline passed is dropped unserved, and a relay whose
upstream did not answer in time says so — the beacon may have pooled the transaction; resending the
same bytes is safe. Until the wire carries it, a relay forwards with half the client's timeout.
**Rationale:** The client and the relay's forward both wait 30 s, expire together, and the client
learns nothing (M4). A deadline (as in gRPC) gives every hop the same clock instead of a guessed
shorter constant per hop.
**Alternatives:** A fixed shorter timeout per hop (the stopgap; wrong as soon as hops or client
timeouts change).

## LR007 — Overload is answered in codes with a retry-after; clients retry the same bytes

**Date:** 2026-10-17
**Decision:** Overload has three answers, as codes, each with a retry-after: the node's queue is
full (`E_BUSY`, like HTTP 503), this peer is over its share (`E_RATE_LIMITED`, like 429 — and the
node announces its per-peer limit so `pp::Client` queues past it locally, like HTTP/2's maximum
concurrent streams), the pool is full (`E_POOL_FULL`, retry-after from the pool's backlog ÷ block
capacity × slot). `pp::Client` submits a transaction with one retry policy: the same signed bytes
(the pool and the chain drop repeats), the retry-after honoured with exponential backoff and jitter,
until the transaction's validity window ends.
**Rationale:** Today the three look different (two texts and a transport error), so no client can
act on them. A retry is safe only with the same bytes: a re-signed transfer carries a new idempotent
id and pays twice — what `pp-client add-tx` does when run again after a timeout (M4).
**Alternatives:** Texts and transport errors as today. Retries left to each client.

## LR008 — Large requests have their own protocol and a node-wide share; the socket is sized for them

**Date:** 2026-10-17
**Decision:** Requests over 64 KiB (blocks, large records) travel on `/pp-ledger/rpc-bulk/1.0.0`;
the small protocol takes nothing larger (Amp resets a channel at the first fragment of a message
over its limit). A node has at most 2 large requests arriving per peer and 8 in all; one more is
answered at once — `E_RATE_LIMITED` / `E_BUSY` with a retry-after — and the client, which binds its
channel when it opens it, reads that before sending its body. A peer outside `allowedPeers` is
refused when its large request's channel opens. Nodes ask for 8 MiB UDP socket buffers
(`udpBufferBytes`), log what the OS granted, warn when it capped them, and report the kernel's drops
on `HEALTH`; hosts raise `net.core.rmem_max` / `wmem_max`.
**Rationale:** A node has one socket and one network thread for every peer. M12: 64 peers sending
4 MiB bodies overflowed the 208 KiB socket buffer (a million datagrams dropped in 8 s), and every
peer lost packets alike — handshakes, health probes, the node's own upstream. The server's queues
were never the bottleneck; bytes arriving were. Bounding the large bodies arriving at once bounds
those bytes (8 × the senders' windows) whatever the number of peers; a buffer of a few MiB holds
them. Refusing at the open costs a round trip, not 4 MiB. M14: the flood no longer touches anyone
else.
**Alternatives:** Only a larger buffer (any buffer is overrun by enough peers). A node-wide cap on
all requests (small requests are cheap, M9: no need to queue them behind blocks). Refusing the
channel open itself (the client sees a transport error, not `E_BUSY` with a retry-after). Reserved
large slots for miners (DESIGN § Open questions).
