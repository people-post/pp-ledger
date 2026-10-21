# Service API (pp-http `/api/chain/`)

What a service beside the chain — a bank crediting deposits, an exchange — reads, and how it relies on
it. Normative.

## Trust: your own relay

A service runs **its own relay** (`pp-relay`), and pp-http beside it on localhost pointed at that
relay (`--beacon <relay multiaddr>`). The relay is the verifier: it follows the chain from its
genesis, checks every block in full (the slot leader's signature, records, state root, `finalAt`),
the beacon authority, signed tips, certificates and equivocation evidence, as every node does. pp-http
is a stateless translation of the relay's answers to JSON; it keeps nothing and checks nothing of its
own. Its routes answer what the relay holds, so they are as trustworthy as the relay the service runs
— never point it at someone else's node.

pp-http binds `127.0.0.1` by default; keep it on localhost, in the service's pod.

## Following the chain

A service follows blocks in order and credits what it sees once it is final:

1. **Read** `GET /api/chain/blocks?from=H&limit=N` from the last height it holds, oldest first. Each
   block names its `previousHash`: a block that does not link to the hash the service holds for the
   height below means the relay rewound — walk back to the last height whose hash still matches, and
   drop what was recorded above it.
2. **Final**: a block at or below the relay's `finalizedHeight` is final. Above it, blocks may still
   rewind (rarely: a beacon takeover's cutover); below it they rewind only by an operator's emergency
   ([BEACON_AUTHORITY § Rewind](BEACON_AUTHORITY.md#rewind)). Credit what may still rewind only as far
   as the service can take it back.
3. **Stop** crediting, for a person to look, when:
   - a rewind drops a block at or below the `finalizedHeight` the service last read;
   - the status shows `equivocation` (the beacon key signed two blocks at one height; the relay halts
     above it until a later generation is certified);
   - the status shows a `certificate` of a later `generation` than the tip's, with a `cutoverHeight`
     (a takeover under way: blocks above the cutover go).

Transfers are `T_DEFAULT` (any token; an issuer sending its own token issues it) and the native
funding of a new account (`T_NEW_USER`). An account ended (`T_END_USER`, its renewal unpaid) leaves
every balance to the recycle account. Fees and renewals move only native balances; no other record
moves a balance.

## Routes

JSON. **Heights, ids, amounts and times are decimal strings** (64-bit values do not survive a JSON
number). Errors are `{"error": "<message>"}`: `400` for a bad request, `502` when the relay does not
answer, `404` for an account it does not hold. Hashes are lowercase hex.

### `GET /api/chain/status`

| Field | Meaning |
|-------|---------|
| `networkId` | The chain's network |
| `nextBlockId` | One above the relay's last block |
| `finalizedHeight` | The highest final block (from headers' `finalAt`); `null` before any |
| `finalitySeconds` | The bound in force |
| `tipHeight`, `generation` | The beacon's signed tip the relay holds, and its generation |
| `certificate` | `{generation, cutoverHeight}` of the latest certificate the relay holds (`cutoverHeight` `null`: none); absent at generation 1 |
| `equivocation` | `{generation, height}` when the relay holds evidence; absent otherwise |

### `GET /api/chain/blocks?from={height}&limit={n}`

Blocks `[from, from + n)` the relay holds (`n` 1–100, default 20; fewer at its tip), with `status` as
above, read first.

| Block field | Meaning |
|-------------|---------|
| `height`, `hash`, `previousHash` | The block, and the one it links to |
| `slot`, `timestamp` | Its slot, and its seal time (Unix seconds, just after the slot ends) |
| `finalAt` | Unix seconds from which it is final |
| `transfers` | `{index, type ("transfer" or "new-account"), tokenId, from, to, amount, fee}` in record order; `index` is the record's position in the block |
| `ended` | `{index, account}`: accounts ended in it |

### `GET /api/chain/accounts/{id}`

`{id, balances: {tokenId: amount}}` in the relay's tip state.

## Compatibility

Routes change only by adding fields and routes; services ignore what they do not know.
