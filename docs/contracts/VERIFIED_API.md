# Verified API (pp-http `/v1/`)

What a service beside the chain may rely on — a bank crediting deposits, an exchange — without linking
C++ or believing any node. Normative. pp-http serves it when started with a pinned genesis
(`--genesis-hash`); the library is `pp_gateway` (`src/gateway/`). Its other routes (`/api/…`) pass
node answers on unverified and are not part of this contract.

Why it exists and what it replaced: [projects/finality](../../projects/finality/DESIGN.md) (FN004–FN006).

## What it checks

As `pp-client verified-account` does ([WIRE_SCHEMA § Account proofs](WIRE_SCHEMA.md#account-proofs)),
each step only on what the previous one proved:

1. **Block 0** hashes to the pinned genesis hash.
2. **The beacon authority**: at its first start, the node's `AUTHORITY_PROOF` up to the node's tip,
   replayed from block 0 — every step signed by the admin set in force at its point
   ([BEACON_AUTHORITY § Trust chain](BEACON_AUTHORITY.md)). From then on it follows every block's
   authority records itself.
3. **Signed tips**, by the beacon key in force; a later generation's certificate the node holds
   (STATUS `certificate`, admin-signed) and equivocation evidence (STATUS `equivocation`) are taken
   first, as a relay takes them, and bound which tips it accepts.
4. **Every block** from the one it holds to the signed tip, bottom up: each hashes to its hash and
   links to the one below; nothing is kept until the tip verifies over all of them.
5. **Accounts**, by `ACCOUNT_PROOF` against a block it verified (its state root).

It starts from the node's tip at its first start (blocks below are not fetched) and keeps what it
verified in its data directory: a restart resumes there.

**Trust.** The pinned hash is taken on faith, and — once, at the first start — the node's authority
proof below the start is taken as complete, as `pp-client verified-account` takes it. Every step in it
is checked; a step left out cannot be seen. Leaving out a change of the admin set or the beacon makes
the gateway refuse what follows (it stops; it is not misled). Leaving out a config update that raised
the finality bound would make blocks look final early: at the same tip, the gateway compares the bound
it followed with the one the node reports and, if the node's is larger, holds its finalized height and
says so (`finalityHeld`) — but a node lying consistently is not caught. Follow a node the service's
operators run (their relay), and start the gateway when the chain's config is known. (A fix belongs
in the chain: the config in force committed to the state root, provable like an account.)

## Finality

A block's **finality time** is its slot's start plus the `finalitySeconds` in force at its epoch
([WIRE_SCHEMA § Config updates](WIRE_SCHEMA.md#config-updates)): the bound genesis set, changed by the
config updates it followed (each from the epoch after its block's). The **finalized height** is the
gateway's own, by its own clock: the highest block that, with every block below it, is past its
finality time. Blocks below its start count once the latest of their possible finality times has
passed. It never trusts the node's STATUS `finalizedHeight`.

Nodes rewind on their own only above the finalized height; deeper is an operator's emergency
([BEACON_AUTHORITY § Rewind](BEACON_AUTHORITY.md#rewind)). A service may treat a final block as
settled; the `rewind` event's `crossedFinality` says when that rare emergency happened.

## Balances

A service watches accounts (`PUT /v1/watch/{id}`). For each verified block, the gateway finds the
watched accounts its records name (the index's rule, `RecordHandler::matchesWalletForIndex`) and
proves them; it also proves every watched account again every sweep (`--v1-sweep-seconds`). A proof
shows an account after the block it is of; nodes prove only at their tip, so:

- Named by one block since the last proof: its balances hold **from that block** on.
- Named by several: they hold from the last; between the first and the last they are **not known**
  (`409`).
- Changed with no block naming it (found by a sweep): they hold from the proof's block; since the
  previous proof they are not known; an `unexplained` event says so.
- A height above the account's last proof, with a block naming it at or below that height: **not proven
  yet** (`409`, ask again).

An account's balances are known from its first proof on; before it, `409`. A rewind drops what was
recorded above the cutover.

## Routes

JSON. Hashes are hex; **account ids and amounts are decimal strings** (64-bit values do not survive a
JSON number); heights, slots and times (Unix seconds) are numbers. Errors are
`{"error": "<message>"}` with the status below. With a token (`--v1-token`), every `/v1/` request
carries `Authorization: Bearer <token>`, else `401`.

| Status | Means |
|--------|-------|
| `400` | A malformed request |
| `404` | Not watched; a block below those the gateway holds |
| `409` | Not yet: not verified, not proven, not final, not known at that height. Ask again later |
| `503` | Not following the chain yet (starting, node unreachable) |

### `GET /v1/tip`

The latest verified block.

```json
{"height": 1042, "hash": "…", "slot": 2210, "slotTime": 1792483050, "finalityTime": 1792569450,
 "final": false, "generation": 2, "finalizedHeight": 731, "finalitySeconds": 86400}
```

`finalizedHeight` is `null` while no block is final. `halted` (a string) is present while the gateway
follows no further: an equivocation (until a later generation is certified) or a fork (below).
`finalityHeld` (a string) is present while the finalized height is held (§ What it checks, Trust).

### `GET /v1/blocks/{height}`

A verified block's facts: `/v1/tip`'s first six fields. `409` above the tip, `404`
below the oldest block held (`--v1-keep-days`: final blocks older than that are forgotten).

### `PUT /v1/watch/{accountId}`, `DELETE /v1/watch/{accountId}`, `GET /v1/watch`

Start watching (idempotent): the next round proves it. Stop: what was recorded for it is forgotten.
`GET` lists `{"accounts": ["<id>", …]}`.

### `GET /v1/accounts/{accountId}/balances?at=…`

```json
{"accountId": "1073741825", "height": 731, "present": true, "balances": {"0": "1010"}}
```

`at`: `final` (the finalized height), `tip` (default), a height, or `time:<unix seconds>` — the last
block whose slot started at or before then (a service's own wait, e.g. a minute after a deposit).
`present: false`: no such account at that height (no balances). Token ids are the keys.

### `GET /v1/events?after={seq}&limit={n}`

```json
{"events": [{"seq": 4, "kind": "rewind", "generation": 2, "keptUpTo": 1039, "droppedUpTo": 1042,
             "crossedFinality": false}]}
```

In order, from `after` (exclusive; default 0), at most `limit` (1–1000, default 100). Kept across
restarts. Kinds:

| Kind | Fields | Means |
|------|--------|-------|
| `certificate` | `generation`, `cutoverHeight` (absent: none) | A later beacon generation is certified (held, not on chain yet) |
| `rewind` | `generation`, `keptUpTo`, `droppedUpTo`, `crossedFinality` | Blocks above `keptUpTo` are gone; balances recorded above it dropped. `crossedFinality`: a final block was among them |
| `equivocation` | `generation`, `height` | That generation signed two blocks at `height`: nothing above it is followed until a later generation is certified |
| `forked` | `generation`, `height` | A certificate vouches for another block at `height` than the one verified here: the gateway stops. Its operators decide; a new data directory starts afresh |
| `unexplained` | `accountId`, `unknownFrom`, `height` | The account changed with no record seen naming it: not known from `unknownFrom` to `height - 1` |

A service stops crediting and paying out on `rewind` with `crossedFinality`, `equivocation` and
`forked` until a person has looked.

## Running it

Beside the service, one per service (FN006): a sidecar on localhost
([SETUP § Verified API](../ops/SETUP.md#verified-api-v1)). Flags and variables:
[CONFIGURATION](../ops/CONFIGURATION.md). It follows one node (`--v1-node`, default `--beacon`): a
relay is the usual one. Its data directory holds `journal.bin` (what it verified, appended; rewritten
from what is held as it grows) and `genesis.hash` (it refuses a directory of another chain).

`/v1/` changes only by adding fields, routes and event kinds; services ignore what they do not know.
