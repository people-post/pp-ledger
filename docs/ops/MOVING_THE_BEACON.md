# Moving the beacon to another host

Case 3 of the beacon authority ([BEACON_AUTHORITY.md](../contracts/BEACON_AUTHORITY.md)): every key
is safe and the beacon must run somewhere else — new hardware, another provider. It is plain
operations: the same key and the same generation move with the work directory, and no admin key is
needed. Drilled by `./scripts/test/pp_ledger_local_test.sh run --suite move` (L-SMOKE-MOVE).

Not this runbook:

- **The key may be stolen** — case 2: the admins certify a new key on one of the maintainer's relays
  ([CONFIGURATION.md § Promoting a relay](CONFIGURATION.md#promoting-a-relay-case-2-the-beacon-key-was-stolen)).
- **Every key lost** (beacon, admin, and a recovery set that cannot act) — case 1, a new network from
  the old one's state ([RE_GENESIS.md](RE_GENESIS.md)).
- **A new key on the same chain, at the operator's choice** — a rotation: stage
  `keys/beacon-signing.next.*`, the admins certify it as the next generation (no cutover), and the
  beacon switches when the certificate commits. It may be combined with a move, but need not be.

## What moves

The whole work directory, dotfiles included, after a clean stop:

| Path | Why |
|------|-----|
| `.signature` | Marks the directory as the beacon's; without it a non-empty directory is refused |
| `config.json` | `port`, `allowedPeers` (or the same `PP_LEDGER_PORT` / `PP_LEDGER_ALLOWED_PEERS`) |
| `keys/beacon-signing.key`, `.pub` (and `.next.*` if staged) | The signing key (or the same `PP_BEACON_SIGNING_KEY[_FILE]`) |
| `keys/amp-identity.txt` | The network identity: the **peer id** relays dial (or the same `PP_LEDGER_IDENTITY_KEY[_FILE]`). Without it a new identity is created and every multiaddr naming the beacon breaks |
| `data/ledger/` | The chain |
| `data/signed-tip.dat` | The last tip it signed: the beacon never signs another block at or below it. A copy without it is refused (below) |
| `data/beacon-cert.dat`, `data/equivocation.dat`, `data/rewind.dat`, `data/rewound.dat` | State it holds, when present |

Logs, `listen.multiaddr` and `init-config.json` need not move. Nothing in the directory names the
host: paths are relative to `-d`, and the node binds every address on its port. A container moves
its `/data` volume and keeps the same secrets.

## Before: prepare the new host and the relays

1. **New host.** The same pp-ledger release; the UDP port open; the clock synchronised (NTP —
   relays and miners calibrated to the old host's clock and do not recalibrate on a move within one
   generation; check `timedatectl` or `chronyc tracking` on both hosts).
2. **Its address.** The peer id moves with `keys/amp-identity.txt`, so the new multiaddr is the old
   one with the new IP and port: `/ip4/<new ip>/udp/<port>/adp/1.0.0/p2p/<same peer id>`
   (`pp-client peer-id <dir>/keys/amp-identity.txt` prints the peer id). If the beacon keeps its
   address (the IP moves with it), or relays name it by host (`/dns4/<host>/…`) and the name is
   pointed at the new host, skip step 3: relays look the name up again when a dial fails (lower the
   record's DNS TTL beforehand, so the change is seen at once).
3. **Relays.** Add the new multiaddr to every relay that follows the beacon as a second candidate
   — `beacons` in `config.json` (beside `beacon`) or `PP_LEDGER_UPSTREAMS` — and restart them one at a
   time. A relay with a single candidate never fails over: it would keep dialling the old address.
   The beacon's `allowedPeers` needs no change (the relays keep their peer ids). Miners talk to the
   relays: nothing to do.

## The move

1. **Note the tip.** `pp-client -b --host <relay> status` — `Signed tip: block N, generation G`.
2. **Stop the old beacon** cleanly: SIGTERM (`systemctl stop`, `docker stop`), and wait for it to
   exit with status 0. Its ledger and signed tip are then on disk. From here no block commits:
   relays serve reads, and miners' blocks are refused until the beacon is back.
3. **Retire the old host before anything else**: disable its service and remove its signing key
   (`keys/beacon-signing.key` or the secret). It must never run again with this key — see below.
4. **Copy the directory** to the new host, preserving dotfiles: `rsync -a <dir>/ <new host>:<dir>/`
   (or `cp -a`; or attach the volume).
5. **Start the new beacon**: `pp-beacon -d <dir>`. Its log shows `Signed tip: block M, generation G`
   with M ≥ N. `pp-client --host-file <dir>/listen.multiaddr health --ready` answers ready.
6. **Watch the relays move.** Each logs `Following upstream [i] <new multiaddr>` within about a
   minute and a half (a dial that failed before the new host was up waits 30 s; relays re-rank every
   minute and at once when a sync fails), and `pp-client -b --host <relay> status` shows the chain
   growing under the same generation G. Clients resubmit transactions that were pending in the old
   beacon's pool; miners re-register within a minute on their own.

Afterwards, drop the old address from the relays' candidates at their next restart (a dead
candidate costs one `STATUS` attempt a minute).

**Keep the outage short.** The first block after it skips every slot since the last. A miner that
was already due (no block for `minerDueAfterSlots`, a day by default) and is elected in those slots
is charged for them, as for any outage; a move of minutes charges nobody in practice.

## Never

- **Run both hosts.** Two beacons with one key sign different blocks at one height sooner or
  later: every node halts on that evidence ([§ Equivocation halt](../contracts/BEACON_AUTHORITY.md#equivocation-halt))
  until the admins certify a new generation. Retire the old host before starting the new one.
- **Copy a running beacon**, or restore an older backup. The beacon refuses a ledger that ends below
  its `data/signed-tip.dat` (`restore the ledger it signed`); do not lean on that — stop it first.
- **Leave `data/signed-tip.dat` behind.** A beacon holding the key in force, with blocks past genesis
  and no signed tip, refuses to start: the ledger it was given may be older than the tip the last
  host signed, and signing it would sign those heights again. Copy the file. Only if it is lost, and
  the ledger certainly holds every block this key signed (copied after a clean stop of the last host
  that ran it), start once with `pp-beacon -d <dir> --accept-ledger-tip`. If in doubt, treat it as
  case 2: the admins certify a new key with a cutover at the last block every node agrees on.
