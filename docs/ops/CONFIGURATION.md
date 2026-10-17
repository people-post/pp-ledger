# Configuration reference

The one place that lists every config field. Each file is JSON in the role's
work directory (`-d <dir>`). Omitted optional fields take the defaults below;
anything not listed here is fixed in code. Every `config.json` field a deployment sets, and the
command-line options, can come from the environment instead ([Environment](#environment)).

| File | Role | Read when |
|------|------|-----------|
| `init-config.json` | beacon | `pp-beacon --init --genesis-key …` only (genesis); a missing file is written as a template. `--init` refuses a work directory whose `data/` already holds a chain (`--force` replaces it, destroying it); a failed `--init` leaves no `data/` behind |
| `config.json` | beacon, relay, miner | every start, optional when the environment sets what the role needs ([Environment](#environment)). With neither, the beacon runs on its defaults (nothing is written); the relay and miner write a template (upstream, account id, keys cannot be guessed) and stop |
| `keys/amp-identity.txt` | beacon, relay | every start; created on first start — unless `PP_LEDGER_IDENTITY_KEY` sets the identity |
| `keys/beacon-signing.key`, `.pub` | beacon | `--init`; created when absent (or pre-generated with `pp-client keygen -o keys/beacon-signing`). Every start: both must be present |
| `keys/beacon-signing.next.key`, `.pub` | beacon | Optional, every start: a key staged for a rotation ([Identity keys](#identity-keys)) |
| `data/signed-tip.dat` | beacon | every start; rewritten with each block. Not config: the latest signed tip ([BEACON_AUTHORITY.md § Signed tip](../contracts/BEACON_AUTHORITY.md#signed-tip)). Keep it with the ledger; do not delete or copy it between beacons |
| `data/rewind.dat`, `data/rewound.dat` | beacon, relay, miner | Not config: a rewind ordered before a restart (applied and removed at the next start) and the last one applied ([LEDGER_STORAGE.md § Node state files](../contracts/LEDGER_STORAGE.md#node-state-files)). Do not edit |
| `data/beacon-cert.dat` | beacon, relay, miner | every start, when present. Not config: the highest beacon certificate seen out of band, not on chain yet ([BEACON_AUTHORITY.md § Certificates out of band](../contracts/BEACON_AUTHORITY.md#certificates-out-of-band)); removed once the chain commits it. Deleting it makes the node forget a cutover it enforces |
| `data/equivocation.dat` | beacon, relay, miner | every start, when present. Not config: equivocation evidence the node halts on ([BEACON_AUTHORITY.md § Equivocation halt](../contracts/BEACON_AUTHORITY.md#equivocation-halt)); removed once a later generation is certified. Deleting it makes the node forget evidence it halts on (its upstreams may serve it again) |

## Environment

Variables override the matching `config.json` field (the file may be absent) and pass the same
checks. A secret marked `[_FILE]` can instead be given as a file path in `NAME_FILE` (Docker /
Kubernetes secrets); setting both is an error. Lists take commas or newlines.

| Variable | Role | Sets |
|----------|------|------|
| `PP_LEDGER_WORK_DIR` | beacon, relay, miner | `-d` |
| `PP_LEDGER_LOG_LEVEL` | all, pp-http | `--log-level`: `debug`, `info` (default), `warning`, `error` (`--debug` = `debug`) |
| `PP_LEDGER_LOG_FILE` | beacon, relay, miner | `--log-file`: also log to this file (relative: to the work directory); `none`: stderr only, for a container's log driver. Default `<role>.log` in the work directory |
| `PP_LEDGER_PORT` | beacon, relay, miner | `port` |
| `PP_LEDGER_ALLOWED_PEERS` | beacon, relay, miner | `allowedPeers` (list) |
| `PP_LEDGER_UPSTREAMS` | relay, miner | `beacons` (list of upstream multiaddrs) |
| `PP_LEDGER_NETWORK_ID`, `PP_LEDGER_GENESIS_HASH`, `PP_LEDGER_TRUSTED_CHECKPOINT`, `PP_LEDGER_PREDECESSOR_DIR` | relay, miner | `networkAnchor.networkId`, `.genesisHash`, `.trustedCheckpointId`, `.predecessorDir` |
| `PP_MINER_ID` | miner | `minerId` |
| `PP_MINER_KEYS` `[_FILE]` | miner | `keys` (list: hex keys or key file paths) |
| `PP_GENESIS_KEY_FILES` | beacon `--init` | `--genesis-key` (list of key files; keeps keys out of argv) |
| `PP_LEDGER_IDENTITY_KEY` `[_FILE]` | beacon, relay | the network identity (hex private key) instead of `keys/amp-identity.txt`: a stable peer id, nothing written |
| `PP_BEACON_SIGNING_KEY` `[_FILE]` | beacon (`--init` and every start) | the beacon signing key (hex private key) instead of `keys/beacon-signing.key` / `.pub`; its public key is derived. Nothing is written |
| `PP_BEACON_SIGNING_NEXT_KEY` `[_FILE]` | beacon | a key staged for a rotation, instead of `keys/beacon-signing.next.*` |
| `PP_HTTP_PORT`, `PP_HTTP_BIND`, `PP_HTTP_CORS_ORIGIN`, `PP_HTTP_BEACON`, `PP_HTTP_MINER` | pp-http | `--port`, `--bind`, `--cors-origin`, `--beacon`, `--miner` |
| `PP_HTTP_METRICS_TARGETS` | pp-http | `--metrics-target` (list of `NAME=MULTIADDR`: more nodes on `/metrics`) |

A command-line option given explicitly wins over its variable. Peer ids can be worked out before
anything starts: `pp-client peer-id <key>` prints the peer id and public key of a private key (hex
or a key file), so upstream multiaddrs can be written ahead. For example, a relay with no files:

```bash
PP_LEDGER_WORK_DIR=/data PP_LEDGER_UPSTREAMS=/ip4/10.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon peer id> \
  PP_LEDGER_LOG_LEVEL=info pp-relay
```

## Health and metrics

Nodes have no HTTP server (no HTTP library is linked into them; [SERVER.md](../architecture/SERVER.md#rpc-requests)).
Health is a ledger RPC, `HEALTH`, which every node answers to any peer at once, from a snapshot its
server thread publishes about once a second: it never waits in the request queue, and is not subject
to `allowedPeers` (it tells a caller only what `STATUS` does, plus readiness). HTTP is pp-http's job.

| Field | Meaning |
|-------|---------|
| `role`, `peerId`, `networkId` | Identity |
| `ready`, `notReady` | Ready to serve, else why not: `starting`, `not synced from the upstream`, `N blocks behind the upstream`, `halted on equivocation evidence`, `stood down…`. Beacon: running, not stood down or halted. Relay: synced and at most 3 blocks behind its upstream's signed tip. Miner: synced |
| `reportAgeMs` | Since the server thread published. Live: under 30 s (`NodeHealth::live()`); a stuck server thread shows here |
| `nextBlockId`, `upstreamNextBlockId`, `lastSyncAgeSeconds`, `signedTipHeight`, `generation`, `halted`, `recoveryPending`, `standingDown`, `requestQueueDepth` | Chain and node state, for metrics |
| `requests` (per type: `served`, `waitMs`, `serviceMs`), `requestsBusy`, `requestsRefused`, `requestsExpired` | Load, counted from the node's start: each request once — served (queue wait and handler time summed), busy (queue full), refused unserved (too large, peer not allowed, malformed, unsupported, wrong direction) or expired (its client had given up) |
| `loopTurnsSlow`, `loopTurnMaxMs`, `rssBytes` | Run-loop turns over a second (a slow handler or duty delays the node's own work), the longest turn of the last 10–20 s, resident memory (Linux) |
| `blocksSealed`, `blocksCommitted`, `blocksAbandoned`, `lastSealDelayMs` | Miner: blocks it produced, committed on the beacon's signed tip, taken by no upstream; how long after its slot ended it sealed the last |
| `txPoolRecords`, `txPoolBytes`, `txPoolOldestAgeSeconds`, `txPoolAdded`, `txPoolDuplicates`, `txPoolFull`, `txPoolSenderFull`, `txPoolExpired`, `txPoolIncluded`, `txPoolDropped` | Beacon: its transaction pool now, and what became of submitted transactions (`SenderFull`: its payer had its share pooled; `Dropped`: could no longer apply after a block) |

A node answers `HEALTH` from the moment it listens, before a relay or miner's startup sync: a node
still syncing is live but not ready. It writes its listen multiaddr (loopback for a wildcard bind)
to `<work dir>/listen.multiaddr`, so a probe on the same host or in the same container needs no
address:

| Tool | Use |
|------|-----|
| `pp-client --host-file <work dir>/listen.multiaddr health` | Prints the JSON; exit 0 when live. `--ready`: exit 0 only when also ready. The image's `HEALTHCHECK`; a Kubernetes `exec` liveness / readiness probe |
| pp-http `GET /api/beacon/health`, `GET /api/miner/health` | The JSON of its `--beacon` / `--miner`: `200` when the node answered (see `ready`), `502` when not |
| pp-http `GET /metrics` | Prometheus text for its `--beacon` (`node="beacon"`), `--miner` (`node="miner"`) and each `--metrics-target NAME=MULTIADDR` (`node="NAME"`), asked in parallel: `pp_ledger_info{node,role,peer_id,network}`, `pp_ledger_up` (0 when the node did not answer or is not live), `pp_ledger_ready`, `pp_ledger_next_block_id`, `pp_ledger_upstream_next_block_id`, `pp_ledger_sync_lag_blocks`, `pp_ledger_last_sync_age_seconds`, `pp_ledger_signed_tip_height`, `pp_ledger_beacon_generation`, `pp_ledger_halted`, `pp_ledger_recovery_pending`, `pp_ledger_standing_down`, `pp_ledger_request_queue_depth`, `pp_ledger_report_age_seconds`; load counters `pp_ledger_requests_total`, `pp_ledger_request_wait_seconds_total`, `pp_ledger_request_service_seconds_total` (label `type`, e.g. `tx_add`), `pp_ledger_requests_busy_total`, `_refused_total`, `_expired_total`, `pp_ledger_loop_turns_slow_total`, `pp_ledger_loop_turn_max_seconds`, `pp_ledger_resident_memory_bytes`; miner `pp_ledger_blocks_sealed_total`, `_committed_total`, `_abandoned_total`, `pp_ledger_last_seal_delay_seconds`; beacon `pp_ledger_tx_pool_records`, `_bytes`, `_oldest_age_seconds`, `pp_ledger_tx_pool_added_total`, `_duplicates_total`, `_full_total`, `_sender_full_total`, `_expired_total`, `_included_total`, `_dropped_total` |

Prometheus scrapes pp-http, not the nodes; one pp-http can watch a whole deployment with
`--metrics-target` (it needs no `allowedPeers` entry for `HEALTH`).

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
| `maxTransactionsPerBlock` | 10240 | Records a block may hold, renewals included (due renewals that do not fit wait) |
| `maxBlockBytes` | 4 MiB | Bytes a block may take, as stored and sent; at most 4 MiB (what the RPC carries) and room for the largest record: `maxCustomMetaSize` + 320 KiB ([WIRE_SCHEMA.md § Block size](../contracts/WIRE_SCHEMA.md#block-size)) |
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

**Re-genesis** (`pp-beacon --init --import-state state.snap`, case 1; runbook
[RE_GENESIS.md](RE_GENESIS.md), rules [BEACON_AUTHORITY.md § Importing genesis](../contracts/BEACON_AUTHORITY.md#importing-genesis)):
`networkId` (new, not the snapshot's), `systemAccounts` (the new keys) and `recovery` are required
as above; `genesisMiners` is refused (miners come with the import). Every other field is the old
network's, from the snapshot, unless set here.

## Beacon `config.json`

```json
{ "port": 8517 }
```

| Field | Default | Meaning |
|-------|---------|---------|
| `port` | 8517 | UDP listen port |
| `allowedPeers` | anyone | Peer ids served (see below); typically the beacon's relays |

Moving the beacon to another host (all keys safe) is plain operations:
[MOVING_THE_BEACON.md](MOVING_THE_BEACON.md).

### Promoting a relay (case 2: the beacon key was stolen)

When the beacon's key is stolen, the admins certify a new key held on one of the maintainer's relays
([BEACON_AUTHORITY.md § Taking over](../contracts/BEACON_AUTHORITY.md#taking-over)):

1. On the chosen relay's host: `pp-client keygen -o <relay dir>/keys/beacon-signing` — the new
   beacon's key pair; send the `.pub` to the admins.
2. The admins: `pp-client -b --host <relay> mk-beacon-cert --network-id <id> --generation <N+1>
   --pubkey beacon-signing.pub --cutover <last trusted block> --dial-hint <gateway relay> -o cert.tx`,
   then `sign-tx` with enough admin keys. The gateways are relays that will follow the new beacon.
3. `pp-client -b --host <relay> submit-tx cert.tx` to the maintainer's relays (the chosen one too).
   Every node rewinds to the cutover and the old beacon stands down.
4. Stop the chosen relay and start `pp-beacon -d <relay dir> --promote`. It checks the directory and
   the host's clock, keeps `config.json` as `config.relay.json` and writes a beacon's (`port`,
   `allowedPeers` kept), and takes over. Later starts are plain `pp-beacon -d <relay dir>`.
5. Point the gateways at it (their `beacon` / `beacons`). The next slot leader commits the
   certificate; relays and miners follow the new generation through the gateways.

## Relay `config.json`

```json
{ "beacon": "/ip4/10.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon peer id>" }
```

| Field | Default | Meaning |
|-------|---------|---------|
| `beacon` | **required** (or `beacons`) | Upstream multiaddr (the beacon or another relay), ending in `/p2p/<peer id>` |
| `beacons` | none | More upstream multiaddrs, after `beacon`; the relay follows the best of them (below) |
| `port` | 8519 | UDP listen port |
| `allowedPeers` | anyone | Peer ids served; typically the relay's miners and clients |
| `networkAnchor.networkId`, `.trustedCheckpointId` | none | An upstream serving another network, or a checkpoint below this one, is not followed (as for a miner) |
| `networkAnchor.genesisHash` | none | Hex hash of the genesis this relay syncs from (as `pp-client block 0` prints it); without it the relay takes genesis from its upstream on trust |
| `networkAnchor.predecessorDir` | none | After a re-genesis: this node's work directory on the old network (stopped). The new genesis must name block *H* of that ledger — same network, block hash and state root — or the node refuses to follow it, and refuses when that ledger does not hold *H* ([BEACON_AUTHORITY.md § Importing genesis](../contracts/BEACON_AUTHORITY.md#importing-genesis)). Checked when syncing block 0 and at every start |

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
| `beacons` | **required** (or `beacon`) | Upstream multiaddrs (relays, or the beacon), each ending in `/p2p/<peer id>`; `beacon` (one) is read as for a relay |
| `port` | 8518 | UDP listen port (miners only dial out; nothing needs to reach it) |
| `networkAnchor` | none | Optional pins checked against upstreams: `{"networkId", "genesisHash", "trustedCheckpointId", "predecessorDir"}` (`predecessorDir`: as for a relay above; a miner fetches block 0 to check it, even when starting at a checkpoint). `genesisHash` is the hex block hash (as `pp-client block 0` prints it, `0x` optional). With it, a fresh miner starting at a checkpoint replays the upstream's `AUTHORITY_PROOF` from genesis and requires the checkpoint to carry the authority it reaches, else starts at genesis |
| `allowedPeers` | anyone | Peer ids served (a miner serves reads to local clients) |

Relays and miners read the same fields the same way: `beacon` and/or `beacons` (in order, a
repeat dropped), `port`, `allowedPeers` and `networkAnchor`. A node also refuses an upstream that
serves another network than the chain it holds. Relays and miners follow the upstream whose signed tip has the highest beacon generation they could
verify, then the longest chain, and re-rank every minute, on a failed sync and on a new beacon
certificate. A beacon certificate's dial hints (`mk-beacon-cert --dial-hint`) are added as upstreams
once the node has loaded its chain — configure at least one upstream reachable at start
([BEACON_AUTHORITY.md § Upstreams](../contracts/BEACON_AUTHORITY.md#upstreams)).

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
`T_BEACON_CERT` for it ([BEACON_AUTHORITY.md § Later generations](../contracts/BEACON_AUTHORITY.md#later-generations--t_beacon_cert-record-18)):

```bash
pp-client keygen -o keys/beacon-signing.next                  # on the beacon host; restart pp-beacon
pp-client mk-beacon-cert --network-id <id> --generation <current + 1> \
  --pubkey keys/beacon-signing.next.pub -o cert.tx
pp-client sign-tx -k admin1.key --network-id <id> cert.tx   # each admin, offline, until the quorum
pp-client submit-tx -b <relay or beacon> cert.tx
```

From the block that commits the certificate the beacon signs with the next key, and relays and
miners follow (`pp-client status` shows the new generation). At a later restart the operator may
move `beacon-signing.next.*` over `beacon-signing.*`. A certificate for a key the beacon does not
hold makes it stand down (reads only): the network waits for the beacon that holds that key.

No `--cutover` here: the current key is not suspect, so it signs until the certificate commits. A
cutover is for a **stolen key**: `--cutover <the last height you trust>` makes every node that
holds the certificate refuse the old generation's tips above that height, before any block
commits it. `mk-beacon-cert` then needs a node to read that block from — give `-b --host <relay>`
before the subcommand — fills in its hash, and prints how many blocks (and hours of chain time) the
cut drops; check that before signing. Add `--dial-hint <relay multiaddr>` (repeatable) for the
relays in front of the new beacon. Hand it to the network with `submit-tx` to any relay, miner or the beacon; each node
it passes keeps it (`data/beacon-cert.dat`) and serves it on `STATUS`, from where the others take
it. **A cutover below a node's tip is a rewind order:** every node holding blocks above it drops
them by restarting itself, unattended, and waits at the cutover
([BEACON_AUTHORITY.md § Rewind](../contracts/BEACON_AUTHORITY.md#rewind)) — when the cutover is at
most seven days of slots back. A deeper cut waits for each node's operator: check the certificate,
then restart the node (`pp-beacon`, `pp-relay`, `pp-miner`) with `--approve-rewind <generation>`.
A node holding another block at the cutover height than the certificate names does not rewind at
all (a fork, or a wrong height): it stops and alerts. Bringing up the new beacon is part 2 of
[beacon-authority](../../projects/beacon-authority/DESIGN.md).

**Replacing the admin keys** (lost or stolen admin keys; the recovery set's only power). The
recovery key holders write, sign and submit a `T_RECOVERY` naming the new admin set — at least 3
keys and a threshold of at least 2 — held apart from the recovery keys
([BEACON_AUTHORITY.md § Recovery](../contracts/BEACON_AUTHORITY.md#recovery--t_recovery-record-19)):

```bash
pp-client keygen -o admin-new1                                # each new admin, offline; likewise 2, 3
pp-client mk-recovery --network-id <id> --admin-pubkey admin-new1.pub --admin-pubkey admin-new2.pub \
  --admin-pubkey admin-new3.pub --min-signatures 2 -o recovery.tx
pp-client sign-tx -k recovery1.key --network-id <id> recovery.tx   # each recovery holder, until its quorum
pp-client submit-tx -b <relay or beacon> recovery.tx
```

Once a block commits it, `pp-client status` shows `Recovery: pending, applies at epoch E`, and every
node logs an alert each epoch until then. Meanwhile the current admin set is frozen: no config
update, no beacon certificate — so a stolen admin key gets nothing out of the wait, and an honest
admin who sees an unexpected recovery has `delayEpochs` to act out of band (the admins cannot
cancel it). From the first block of epoch E the new keys are the admin set.

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
| Relay / miner startup sync (a miner's wait for an upstream to answer included) | 5 min |

The request queue holds 1024 requests (reads get a further quarter in a
low-priority lane, at most 16 per peer) and 64 MiB of request bodies. A peer may have 16 requests
arriving at once; requests other than `BLOCK_ADD` and `TX_ADD` are at most 64 KiB, those two up to
a block (4 MiB) plus 64 KiB ([AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md#protocol)).
