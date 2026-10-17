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
