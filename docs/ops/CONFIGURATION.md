# Configuration reference

The one place that lists every config field. Each file is JSON in the role's
work directory (`-d <dir>`). Omitted optional fields take the defaults below;
anything not listed here is fixed in code.

| File | Role | Read when |
|------|------|-----------|
| `init-config.json` | beacon | `pp-beacon --init --genesis-key …` only (genesis); a missing file is written as a template |
| `config.json` | beacon, relay, miner | every start. If missing, the beacon creates `{"port": 8517}` and runs; the relay and miner write a template (upstream, account id, keys cannot be guessed) and stop |
| `keys/amp-identity.txt` | beacon, relay | every start; created on first start |
| `keys/beacon-signing.key`, `.pub` | beacon | `--init`; created when absent (or pre-generated with `pp-client keygen -o keys/beacon-signing`). Every start: both must be present |
| `keys/beacon-signing.next.key`, `.pub` | beacon | Optional, every start: a key staged for a rotation ([Identity keys](#identity-keys)) |
| `data/signed-tip.dat` | beacon | every start; rewritten with each block. Not config: the latest signed tip ([WIRE_SCHEMA.md § Signed tip](../contracts/WIRE_SCHEMA.md#signed-tip)). Keep it with the ledger; do not delete or copy it between beacons |

## `init-config.json` (beacon, genesis)

```json
{
  "networkId": "my-network",
  "systemAccounts": {
    "genesis":   {"publicKeys": ["<hex>", "<hex>", "<hex>"], "minSignatures": 2},
    "reserve":   {"publicKeys": ["<hex>"]},
    "registrar": {"publicKeys": ["<hex>"]},
    "fee":       {"publicKeys": ["<hex>"]},
    "recycle":   {"publicKeys": ["<hex>"]}
  },
  "recovery": {"publicKeys": ["<hex>", "<hex>", "<hex>"], "minSignatures": 2},
  "genesisMiners": [
    {"id": 1048576, "publicKeys": ["<hex ML-DSA-65 public key>"]}
  ]
}
```

```bash
pp-beacon -d beacon --init --genesis-key genesis1.key --genesis-key genesis2.key
```

**Keys: each holder makes its own** (`pp-client keygen -o <name>` writes
`<name>.key` and `<name>.pub`). The config holds public keys only; the beacon
never generates or stores an account's private key. `--genesis-key` (repeat
for M-of-N) signs the genesis block; the keys must be distinct genesis keys and
at least its `minSignatures`.

**The genesis account is the admin key.** After genesis it only signs config
updates (`T_CONFIG`) and issues new supply to reserve (it may transfer to no
other account and creates no accounts), so it must be M-of-N from the start: at
least 3 keys and 2 signatures (the shape its renewals and updates are held to).
Keep its keys offline with different people; the beacon needs them only for
`--init`. A config update can replace the genesis keys (rotation, immediate);
its config applies at the next epoch boundary, one update at a time
([WIRE_SCHEMA.md](../contracts/WIRE_SCHEMA.md#config-updates)).

**The registrar creates issued accounts** (2²⁰ ≤ id < 2³⁰: token issuers,
operator accounts); no other account can. It starts empty and spends real
tokens that reserve sends it, so its balance bounds what it can create.

**Key custody: keep hot keys away from supply.** Creating user accounts
(id ≥ 2³⁰) needs no system key: any funded account signs its own `NEW_USER`
and pays the new account's balance and fee. Recommended tiers (the accounts'
powers are chain rules; the custody is a practice):

| Tier | Account | Signs | Keys |
|------|---------|-------|------|
| Admin | genesis | config (`T_CONFIG`), issuance to reserve, key rotation | cold, M-of-N |
| Treasury | reserve | occasional top-ups of the registrar and onboarding accounts | cold or warm, M-of-N |
| Registrar | registrar | every issued-range `NEW_USER` | warm, M-of-N |
| Onboarding | an ordinary user or issued account | every user-range `NEW_USER` | hot, small balance |

A leaked onboarding key loses at most that account's balance; stop refilling it
and create another. Run several (per region or service) with separate keys and
budgets rather than signing onboarding with reserve or genesis.

| Field | Default | Meaning |
|-------|---------|---------|
| `networkId` | **required** | This chain's name. Part of genesis: every signature and the epoch seed bind to it; offline signing (`pp-client sign-tx --network-id`) must name it |
| `systemAccounts` | **required** | `genesis`, `reserve`, `registrar`, `fee`, `recycle`: each `{publicKeys, minSignatures?}` (default: all keys must sign). Genesis: ≥ 3 keys, ≥ 2 signatures |
| `slotDuration` | 7 | Seconds per slot |
| `slotsPerEpoch` | 86400 | Slots per epoch |
| `heartbeatSlots` | `slotsPerEpoch` | Empty block when the tip lags this many slots (0 = off; at most `slotsPerEpoch`) |
| `maxTransactionsPerBlock` | 10240 | |
| `maxCustomMetaSize` / `freeCustomMetaSize` | 1 MiB / 1024 | Account meta bytes allowed / free of fee |
| `minFeeCoefficients` | `[1, 1, 1]` | Fee = a + b·x + c·x², x = non-free meta KiB (rounded up) |
| `newAccountFee` | 0 | One-time fee every account creation after genesis pays on top of the above, to the fee account. `0` = none. Creation pays both up front: the meta-size fee (the same formula renewal charges) covers the account's first period, until renewal falls due about a year later; each renewal then pays for the next period |
| `checkpointMinBlocks` / `checkpointMinAgeSeconds` | 2²⁰ / 1 year | Renewal cut-off (accounts last written before it must renew) and checkpoint spacing: a checkpoint is the first block of an epoch at least `checkpointMinBlocks` after the previous one, whose block is more than `checkpointMinAgeSeconds` old ([BLOCK_PIPELINE.md](../architecture/BLOCK_PIPELINE.md#checkpoints-and-late-join)). Both read slot times, not a node's clock |
| `maxValidationTimespanSeconds` | 86400 | Widest allowed transaction validity window |
| `maxIssuancePerEpoch` | 0 | Most native tokens genesis may issue to reserve per epoch (amount + fee, net of anything sent back to genesis). `0` = no issuance until a config update raises it — which itself takes effect only at the next epoch |
| `minerMinStake` | 0 | Native balance a miner needs to register (after its fee) and to be elected |
| `minerRegistrationFee` | 0 | Flat fee for `T_MINER_REGISTER` on top of the meta-size fee, to the fee account |
| `minerDueAfterSlots` | one day of slots (`⌈86400 / slotDuration⌉`) | A miner whose last block (or registration) is this many slots old is due: it must produce, an empty block if need be, in a slot it leads. `0` = never due; otherwise at least 12 hours of slots (`⌈43200 / slotDuration⌉`) |
| `minerMaxMissedSlots` | 3 | Slots a due miner may miss in a row before it is deregistered. `0` = never |
| `minerReregisterCooldownEpochs` | 0 | Epochs an automatically deregistered miner waits before registering again |
| `genesisMiners` | none | Miner accounts created at genesis (below), registered at block 0 |
| `recovery` | **required** | `{publicKeys, minSignatures?, delayEpochs?}`: the key set whose only power is replacing the admin (genesis account) keys, after `delayEpochs` (default: ~30 days of epochs). Keys are held apart and must not be admin keys. Fixed at genesis |

**Genesis miners.** System accounts (genesis, reserve, registrar, fee, recycle) never
lead slots, and only registered miners are elected, so a new chain needs miners from block 0.
Later miners register on chain with `pp-client -b --host <relay> miner-register <account> -f <fee>
-k <key>` (rules: [WIRE_SCHEMA.md § Miner registration and liveness](../contracts/WIRE_SCHEMA.md#miner-registration-and-liveness)). Each entry creates one:

- `id` — in the issued range `[1048576, 1073741824)` (2²⁰ ≤ id < 2³⁰)
- `publicKeys` — hex ML-DSA-65 public keys; the miner keeps the private keys
  (`pp-client keygen`)
- `minSignatures` — optional, default: all keys
- `stake` — optional initial balance taken from reserve; default an equal share
  of 10% of the supply among miners without an explicit stake

## Beacon `config.json`

```json
{ "port": 8517 }
```

| Field | Default | Meaning |
|-------|---------|---------|
| `port` | 8517 | UDP listen port |
| `allowedPeers` | anyone | Peer ids served (see below); typically the beacon's relays |

## Relay `config.json`

```json
{ "beacon": "/ip4/10.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon peer id>" }
```

| Field | Default | Meaning |
|-------|---------|---------|
| `beacon` | **required** | Upstream multiaddr (the beacon or another relay), ending in `/p2p/<peer id>` |
| `port` | 8519 | UDP listen port |
| `allowedPeers` | anyone | Peer ids served; typically the relay's miners and clients |
| `networkAnchor.genesisHash` | none | Hex hash of the genesis this relay syncs from (as `pp-client block 0` prints it); without it the relay takes genesis from its upstream on trust |

## Miner `config.json`

```json
{
  "minerId": 1048576,
  "keys": ["key.txt"],
  "beacons": ["/ip4/10.0.0.2/udp/8519/adp/1.0.0/p2p/<relay peer id>"]
}
```

| Field | Default | Meaning |
|-------|---------|---------|
| `minerId` | **required** | The miner's account id (e.g. a genesis miner) |
| `keys` | **required** | Files with the account's hex private keys; the first is also the miner's network identity |
| `beacons` | **required** | Upstream multiaddrs (relays, or the beacon), each ending in `/p2p/<peer id>` |
| `port` | 8518 | UDP listen port (miners only dial out; nothing needs to reach it) |
| `networkAnchor` | none | Optional pins checked against upstreams: `{"networkId", "genesisHash", "trustedCheckpointId"}`. `genesisHash` is the hex block hash (as `pp-client block 0` prints it, `0x` optional). With it, a fresh miner starting at a checkpoint replays the upstream's `AUTHORITY_PROOF` from genesis and requires the checkpoint to carry the authority it reaches, else starts at genesis |
| `allowedPeers` | anyone | Peer ids served (a miner serves reads to local clients) |

## `allowedPeers` (any role)

An array of peer ids (the `/p2p/` part of a node's multiaddr). When present,
the node serves only those peers and its own upstreams; any other sender gets
"Peer not allowed" before its request is queued. Absent = anyone; `[]` = only
upstreams. Typical: the beacon lists its relays, so only relays reach the
terminal; a relay lists its miners and trusted clients.

## Identity keys

The beacon signs with its own key, `keys/beacon-signing.key` (`.pub` beside it), separate from its
network identity. `--init` creates the pair when neither file exists, else reuses it when both exist
and match; the genesis record certifies its public key as beacon generation 1. Every start loads
it and signs each committed block with it (`pp-client status` shows `Signed tip: block …`).

**Rotating the key** (same server; the next generation). Stage the new pair beside the current
one and restart the beacon so it holds both; then the admins write, sign and submit a
`T_BEACON_CERT` for it ([WIRE_SCHEMA.md § Chain authority](../contracts/WIRE_SCHEMA.md#chain-authority)):

```bash
pp-client keygen -o keys/beacon-signing.next                  # on the beacon host; restart pp-beacon
pp-client mk-beacon-cert --network-id <id> --generation <current + 1> \
  --pubkey keys/beacon-signing.next.pub --cutover <current tip height> -o cert.tx
pp-client sign-tx -k admin1.key --network-id <id> cert.tx   # each admin, offline, until the quorum
pp-client submit-tx -b <relay or beacon> cert.tx
```

From the block that commits the certificate the beacon signs with the next key, and relays and
miners follow (`pp-client status` shows the new generation). At a later restart the operator may
move `beacon-signing.next.*` over `beacon-signing.*`. A certificate for a key the beacon does not
hold makes it stand down (reads only): the network waits for the beacon that holds that key.

The beacon and each relay create `keys/amp-identity.txt` on first start; its
peer id is the `/p2p/` part of their multiaddr, printed at start as
`AMP ledger listener: ...`. A miner's identity is its first account key.

## Network timing (fixed)

Not configurable. Every request deadline derives from one RPC timeout T = 15 s
(`NetworkTuning`), so they cannot drift apart:

| Deadline | Value |
|----------|-------|
| Light requests (status, calibration, register, miner list) | T |
| Data requests (blocks, transactions, accounts) | 2T |
| Server drops a request still queued after | T/2 |
| RPC channel with nothing inbound is reset after | 2T on the server, the call's deadline on the client |
| Upstream holds a `BLOCK_WAIT` for | T/2 |
| Relay / miner startup sync | 5 min |

The request queue holds 1024 requests (reads get a further quarter in a
low-priority lane, at most 16 per peer).
