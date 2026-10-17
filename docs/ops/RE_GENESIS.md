# Re-genesis: a new network from the old one's state

Case 1 of the beacon authority ([BEACON_AUTHORITY.md § Importing genesis](../contracts/BEACON_AUTHORITY.md#importing-genesis)):
no authority is left that the protocol could follow — the beacon key and the admin keys are lost
or stolen, and the recovery set cannot act. A new network starts whose accounts are the old one's at
a chosen block *H*, and every node checks that they are. Drilled by
`./scripts/test/pp_ledger_local_test.sh run --suite regenesis` (L-SMOKE-REGENESIS).

Not this runbook:

- **The recovery set can act** — it replaces the admin set, then case 2 follows
  ([CONFIGURATION.md § Promoting a relay](CONFIGURATION.md#promoting-a-relay-case-2-the-beacon-key-was-stolen)).
  Always preferable: it keeps the network, its history and every signature.
- **All keys are safe** — moving or rotating needs no new network ([MOVING_THE_BEACON.md](MOVING_THE_BEACON.md)).

Nothing on the old chain can order a re-genesis — anything that could would serve a thief too. It is
announced out of band, and each operator decides to follow it. What every node can check is that the
new network's accounts are exactly the old network's at block *H*.

## What carries over, what does not

| Carries over | Does not |
|--------------|----------|
| Every account at *H*: id, balances (every token), keys, threshold, profile | Transactions after *H* — they must be signed again, for the new network |
| Miner registration and stake (slot counters restart) | The transaction history: the old chain stays the archive |
| The old config (slot timing, fees, limits…), but for what `init-config.json` sets | The old network id: every signature binds it, so nothing signed for one network is valid on the other |
| | The system accounts' keys and the admin, recovery and beacon keys: all new |

## The maintainer

1. **Choose *H*.** Keys lost (the chain halted): *H* is the last signed tip — every node has it. Keys
   stolen: the last block everyone trusts, below anything the thief signed (as for a case 2 cutover).
2. **Export** from a full node (a relay: its ledger starts at genesis), stopped:
   `pp-client export-state -d <relay dir> --height H -o state.snap`. It works on a scratch copy and
   writes the snapshot only once the accounts hash to block *H*'s state root.
3. **New keys**, each by its holder (`pp-client keygen -o <name>`): genesis (admin) keys, reserve,
   registrar, fee, recycle, recovery — and the new beacon's signing key (`--init` makes it).
4. **`init-config.json`** in the new beacon's directory: a new `networkId` (not the old one's),
   `systemAccounts` and `recovery` with the new public keys, no `genesisMiners`. Other fields only to
   change the old network's ([CONFIGURATION.md § init-config.json](CONFIGURATION.md#init-configjson-beacon-genesis)).
5. **`pp-beacon -d <dir> --init --import-state state.snap --genesis-key g1.key --genesis-key g2.key`.**
   Block 0 names the predecessor (with block *H*'s header); blocks 1 … *k* import the accounts; the
   tip at *k* is signed. Then start the beacon and its relays (fresh directories).
6. **Announce** out of band: the new network id and genesis hash (`pp-client -b --host <relay> block 0`),
   the predecessor (old network id, *H*, block hash — `export-state` printed them), and the gateways
   (relay multiaddrs).

## Operators of relays and miners

Each starts a **fresh node** for the new network and keeps the old work directory, stopped:

1. **Check it by hand** (optional, also for anyone holding the old chain):
   `pp-client --host <new relay> check-predecessor -d <old work dir>` — block 0 of the new network
   must name block *H* of your old ledger; it prints the new genesis hash to compare with the
   announcement.
2. **Configure** the new node: its upstreams; `networkAnchor.genesisHash` (the announced genesis);
   **`networkAnchor.predecessorDir`** = the old work directory (or `PP_LEDGER_PREDECESSOR_DIR`). The
   node then checks the new genesis against its own ledger every time — when syncing block 0, at
   every relay start, and in a miner fetching block 0 — and refuses to follow a network that does not
   name block *H* of it. A node whose old ledger ends below *H* refuses too (it cannot vouch).
3. **Miners keep their keys and account ids**: the same `minerId` and key files; their registration
   and stake came across.

## Users and clients

Switch the network id (`pp-client sign-tx --network-id`, pp-http's nodes) and the endpoints.
Account ids, balances and keys are unchanged. Sign again, for the new network, anything after *H*
or pending.

## Afterwards

- The old network may go on — a thief holding its keys can keep producing. It is a different network
  id; only the announcement moves people. Keep old nodes stopped (their directories are the
  predecessor check).
- Renewals of the imported accounts fall due together a renewal period after the import; due
  renewals that do not fit a block wait ([WIRE_SCHEMA.md § Config updates](../contracts/WIRE_SCHEMA.md#config-updates)).
