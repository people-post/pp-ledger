# Finality — a rewind bound on the chain, and a verifying gateway

**Status:** built. The bound (fn1) is normative in [WIRE_SCHEMA § Config updates](../../docs/contracts/WIRE_SCHEMA.md#config-updates) and [BEACON_AUTHORITY § Rewind](../../docs/contracts/BEACON_AUTHORITY.md#rewind); since fn4 each header carries when it is final (FN007). The verifying gateway below (fn2, fn3) was removed (FN008): services follow their own relay ([SERVICE_API.md](../../docs/contracts/SERVICE_API.md)); its sections stay as the record of why it was built. Phases: [PHASES.md](PHASES.md) · decisions: [DECISIONS.md](DECISIONS.md) ·
status: [CURRENT_STATE.md](CURRENT_STATE.md).

Services beside the chain — the bank (`nfsc-brief-cashier/app-cashier`, projects/bank b4), the
exchange — must know **when a block can no longer be undone**: the bank lets a deposit leave its
books only then. Today none can: a beacon takeover rewinds the chain to its cutover
([BEACON_AUTHORITY § Rewind](../../docs/contracts/BEACON_AUTHORITY.md#rewind)), on its own up to seven
days of slot time (`rewind::kMaxAutomaticRewind`), deeper with an operator's approval. And those
services are not written in C++: they need what a pp-ledger client verifies, without linking it.

This project gives the chain a **finality bound** — a block older than it is final, and no node undoes
it on its own — and pp-http a **verified API** a service runs beside itself.

## The finality bound (FN001–FN003)

### In the config (FN001)

`BlockChainConfig.finalitySeconds`: **24 hours** at genesis by default; between **1 hour** and **30
days**. Changed like any config — a `T_CONFIG` signed by the admin set, applied at the next epoch
boundary ([WIRE_SCHEMA § Config updates](../../docs/contracts/WIRE_SCHEMA.md#config-updates)) —
checked with the liveness settings (`chain_tx::validate…`) at `pp-beacon --init` and every `T_CONFIG`.
The floor keeps a change from taking the network's way back from a takeover; the ceiling keeps "final"
meaning something.

### A block keeps its bound (FN002)

A block's **finality time** is its slot's start plus the `finalitySeconds` **in force for that block**
(its epoch's config). A change never reaches back: lowering it lets blocks committed under it become
final sooner; raising it never makes a block that was final undoable again.

A node's **finalized height** is the highest block such that it and every block below it have passed
their finality times (by the node's calibrated clock). It only grows — prefix-closed, so a lowered
bound cannot make a block final before the ones under it.

### What a node rewinds on its own (FN003)

A node rewinds on its own only when **no block it would drop is final**: the cutover is at or above its
finalized height. This replaces the seven-day measure (`kMaxAutomaticRewind`). Deeper — a certificate
whose cutover drops a final block — is the **emergency**, kept: the node refuses the old generation's
tips past the cutover, alerts, and waits for its operator to approve (`--approve-rewind <generation>`),
as today. Whether lost history is recovered is the chain's operators' decision; services that treat
the bound as solid carry that rare risk.

Reported: `STATUS` carries `finalizedHeight` and the `finalitySeconds` in force; `RewindReport` gains
`crossedFinality` (an approved rewind that dropped final blocks).

## The verifying gateway (FN004–FN006)

### pp-http, verified (FN004)

pp-http (`src/app/http.cpp`, `PP_LEDGER_BUILD_HTTP`) is the HTTP face of `pp::Client`; its routes pass
on what nodes answer. It gains a route group, **`/v1/`, that answers only what it verified** — the
way `pp-client verified-account` does
([WIRE_SCHEMA § Account proofs](../../docs/contracts/WIRE_SCHEMA.md#account-proofs)):

1. **Block 0** is the one the configured `genesisHash` names (`verify::checkGenesis`) — required for
   `/v1/`.
2. **The authority**: `AUTHORITY_PROOF` replayed from block 0 (`verify::authorityAt`); certificates and
   equivocation evidence held as a relay holds them.
3. **Signed tips**, checked with the beacon key in force (`AuthorityTrail::verifyTip`); **blocks under
   them**, hash by hash (`verify::checkBlocksUnderTip`).
4. **Accounts**, by `ACCOUNT_PROOF` against the verified block (`verify::checkAccountProof`).

The existing routes stay as they are (unverified, documented as such).

### Balances by height, from proofs (FN005)

A service watches accounts — the bank its deposit accounts. The gateway follows verified blocks; for
each new tip, it finds the watched accounts the new blocks' records name, takes their account proofs at
that tip, and records each one's balances at that height. An account's balance at any height since it
was watched is the last recorded at or below it. It never re-executes records: balances come from the
state root, as a wallet reads them.

Kept in its data directory, so a restart neither loses history nor re-reads the chain. On a rewind (a
held certificate whose cutover is below the gateway's tip), what it recorded above the cutover is
dropped, and the rewind reported.

### Beside each service (FN006)

The gateway runs **beside the service that uses it** — a sidecar in its pod — listening on localhost,
optionally behind a bearer token. Each service verifies for itself: a shared gateway would be one more
thing to believe. Amounts are decimal strings (64-bit values do not survive JSON numbers).

### The API

As designed; built and normative in [VERIFIED_API.md](../../docs/contracts/VERIFIED_API.md), which adds
what building it showed: balances are not known between two writes one proof covers (`409`); a sweep
catches writes no record names (`unexplained`); `forked`; final blocks forgotten after
`--v1-keep-days`; and finality read from headers (FN007), so the trust its first start places in one
node's authority proof cannot move it.

| Route | Answers |
|-------|---------|
| `GET /v1/tip` | `{height, hash, slot, slotTime, generation, finalizedHeight, finalitySeconds}` — the latest verified tip |
| `GET /v1/blocks/{height}` | `{height, hash, slot, slotTime, finalityTime}` — a verified block's header facts |
| `PUT /v1/watch/{accountId}` | Start watching (from the current tip: its proof is the first record). `DELETE` stops |
| `GET /v1/accounts/{accountId}/balances?at=final\|tip\|{height}\|time:{unixSeconds}` | `{accountId, height, balances: {tokenId: "amount"}}` — at the finalized height, the tip, a height, or the last block whose slot started at or before a time (a service's own wait, e.g. a minute); `409` before the account was watched |
| `GET /v1/events?after={seq}` | `{events: [{seq, kind, …}]}` — `rewind` (`keptUpTo`, `droppedUpTo`, `generation`, `crossedFinality`), `equivocation` (the halted height), `certificate` (a new generation) |
| `POST /v1/tx/submit` | A transaction signed elsewhere, passed on (later: withdrawals; the gateway holds no key) |

## Wire

- `BlockChainConfig` gains `finalitySeconds`; the genesis meta's version rises: networks start from a
  new genesis (none is live).
- `STATUS` gains `finalizedHeight`, `finalitySeconds`; `RewindReport` gains `crossedFinality`.
- `pp-beacon --init` / init-config: `finalitySeconds` (default 86400).

## Phases

[PHASES.md](PHASES.md): the bound in the chain (fn1), the gateway (fn2), docs (fn3); the bank's adapter
is its own (projects/bank b4).
