# Finality — decisions

Rationale for [DESIGN.md](DESIGN.md). When behaviour ships, promote it into `docs/contracts/` and mark
the entry **superseded by** that doc.

## FN001 — A finality bound in the chain config

**Date:** 2026-10-20
**Superseded by:** [WIRE_SCHEMA.md § Config updates](../../docs/contracts/WIRE_SCHEMA.md#config-updates) and [BEACON_AUTHORITY.md § Rewind](../../docs/contracts/BEACON_AUTHORITY.md#rewind) (normative; this entry keeps the why).
**Decision:** `BlockChainConfig.finalitySeconds`, 24 hours by default, from 1 hour to 30 days, set at
genesis and changed by `T_CONFIG` like any config.
**Rationale:** Services that move value off the chain need a time after which a block stands; it must
be the chain's (every node and every service agree on one number), not a constant compiled in. The
owner's call: 24 hours for now, changeable once the network runs. The floor keeps a change from removing
the network's way back from a takeover; the ceiling keeps "final" meaning something.
**Alternatives:** The seven-day constant (`kMaxAutomaticRewind`: not the chain's, and approval reaches
past it); a fixed number of blocks (slots, not blocks, measure time: an idle chain makes few blocks).

## FN002 — A block keeps the bound it was committed under

**Date:** 2026-10-20
**Superseded by:** [WIRE_SCHEMA.md § Config updates](../../docs/contracts/WIRE_SCHEMA.md#config-updates) and [BEACON_AUTHORITY.md § Rewind](../../docs/contracts/BEACON_AUTHORITY.md#rewind) (normative; this entry keeps the why).
**Decision:** A block's finality time is its slot's start plus the bound in force for it. The finalized
height is the highest block such that it and every block below it are past their finality times.
**Rationale:** A service relies on "final" once it holds: raising the bound must not make a final block
undoable again. Prefix-closed, a lowered bound cannot make a block final before the blocks under it —
a rewind drops a suffix.
**Alternatives:** The bound in force now for every block (a raise would reopen history).

## FN003 — Nodes rewind on their own only above the finalized height; deeper is an operator's emergency

**Date:** 2026-10-20
**Superseded by:** [WIRE_SCHEMA.md § Config updates](../../docs/contracts/WIRE_SCHEMA.md#config-updates) and [BEACON_AUTHORITY.md § Rewind](../../docs/contracts/BEACON_AUTHORITY.md#rewind) (normative; this entry keeps the why).
**Decision:** A rewind that drops no final block happens on its own, as today's within-seven-days.
One that drops a final block waits for the operator's approval (`--approve-rewind`), alerts, and is
reported (`crossedFinality`).
**Rationale:** The owner's call: recovering history lost to an undetected takeover is a decision for
the chain's human operators, so the emergency stays. Services treat the bound as solid and carry that
rare risk; the report tells them it happened.
**Alternatives:** A hard limit, no approval past the bound (a takeover noticed late could never be
undone).

## FN004 — A verifying gateway in pp-http for services in other languages

**Date:** 2026-10-20
**Superseded by:** [VERIFIED_API.md](../../docs/contracts/VERIFIED_API.md) (normative; this entry keeps the why).
**Decision:** pp-http gains `/v1/`, answering only what it verified from a pinned genesis hash: the
authority, signed tips, blocks under them, account proofs. Its existing routes stay as they are.
**Rationale:** The bank and the exchange are TypeScript. Verification stays in the C++ the ledger tests
and maintains; services get a narrow JSON API. pp-http already is the client's HTTP face.
**Alternatives:** A Node addon binding `pp::Client` (native code inside a money service: a crash is the
service's; per-platform builds; releases tied together); a TypeScript port (two verifiers that drift);
calling `pp-client` (slow, fragile); WebAssembly (later, for browsers).

## FN005 — Balances by height, from account proofs of touched accounts

**Date:** 2026-10-20
**Superseded by:** [VERIFIED_API.md](../../docs/contracts/VERIFIED_API.md) (normative; this entry keeps the why).
**Decision:** The gateway follows verified blocks; for each tip it proves the watched accounts the new
blocks' records name, and records their balances at that height; a rewind drops what it recorded above
the cutover.
**Rationale:** A service needs balances at two heights — final and recent — and nodes prove an account
only at their tip. Proofs give balances from the state root, as a wallet reads them; only touched
accounts are proven, so thousands of watched accounts cost nothing while idle.
**Alternatives:** Proving every watched account at every tip (cost grows with the pool); re-executing
records to track balances (a second implementation of the rules: fees, renewals, recovery).

## FN006 — A gateway beside each service

**Date:** 2026-10-20
**Superseded by:** [VERIFIED_API.md](../../docs/contracts/VERIFIED_API.md) (normative; this entry keeps the why).
**Decision:** The gateway runs in the service's pod, on localhost (optionally a bearer token); its
history in its own data directory.
**Rationale:** Each service verifies for itself; a shared gateway would be one more party to believe,
and one more network path to guard.
**Alternatives:** One shared internal gateway (fewer processes, a trusted middleman).

## FN007 — Each block's header carries when it is final

**Date:** 2026-10-20
**Decision:** The block header gains `finalAt`: the later of the block below's `finalAt` and this block's
slot start plus the `finalitySeconds` in force at its epoch (genesis: its slot start plus genesis's).
The producer sets it at seal; every node checks it where it checks the state root (a block with another
value is refused). A node's finalized height is the highest block whose `finalAt` has passed; the
gateway reads it from the headers it verified.
**Rationale:** Security first (the owner's call). Finality was computed from the bound history, which a
client learns from a node's authority proof: a proof that left out a config update raising the bound
could not be seen, and a gateway would call blocks final early. In the header, every signed tip vouches
for it — no history, no config decoding, nothing to leave out. A running maximum makes it prefix-closed
by construction (FN002), and one header says when everything below it is final, so blocks below a
gateway's start need no guess.
**Alternatives:** The config in force committed to the state root, proven like an account (gives the bound
now, not the bounds of the blocks below a start); `finalitySeconds` alone in each header (a client would
still take the running maximum over blocks it does not hold); keeping the history and cross-checking the
node's STATUS (catches only a node that lies inconsistently).
