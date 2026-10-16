# Ledger block storage

On-disk layout for the append-only block log (hard cut; no migration from the
former recursive `DirDirStore` / `data/` layout), and the node state files kept beside it.
A role's work directory (`-d <dir>`) holds the block log at `data/ledger/` (the `workDir` below).

## Layout

```
workDir/
  ledger_index.dat     # startingBlockId, checkpoints (Ledger meta)
  volumes/
    volumes_idx.dat    # catalog: max caps + {volumeId, startBlockId}*
    v000001/           # FileDirStore (idx.dat + size-capped block files)
    v000002/           # opened empty when previous volume cannot fit
```

## Growth model

| Layer | Cap | Behavior when full |
|-------|-----|--------------------|
| `FileStore` | `maxFileSize` | open next file in the same volume |
| `FileDirStore` | `maxFileCount` | volume cannot fit → next volume |
| `VolumeStore` | `maxVolumes` | append fails |

Existing volumes are never relocated or rewritten for capacity. Only the tip
volume is mutated on append; a rewind (`Ledger::rewindTo`) cuts the file holding its target and
deletes every later file and volume — the index first, then the data, so each step leaves a prefix
of the chain and a crash midway mounts consistently.

Default Ledger caps: 64 files × 128 MiB per volume (~8 GiB), up to 10 000 volumes.

## Durability

"Durable" means synced to the device (`utl::syncFile` / `syncDirectory`: fsync; `F_FULLFSYNC` on
macOS; `FlushFileBuffers` and write-through renames on Windows), so it survives a power loss, not
only a process crash.

- **Commit point.** A block file's header holds the count of its **committed** blocks. Appending a
  block writes its bytes only; committing syncs the data, then writes the count and syncs again
  (`FileStore::commit`), file by file in order. At mount each block file keeps its committed
  blocks and truncates whatever follows (blocks never committed, or a torn write); a file holding
  fewer complete blocks than its header commits is refused. A ledger directory belongs to one
  process: a mount trims the tail another process has not committed.
- **When blocks commit** (`Ledger::CommitPolicy`): the beacon commits every block before it signs
  it, so a signed block always survives; relays and miners commit once 1000 blocks are
  uncommitted or the oldest has waited 5 s, and on a clean stop (SIGINT, or the SIGTERM docker and
  systemd stop with) — a crash or `kill -9` loses at most that tail,
  which they sync again from upstream. Catch-up stays fast (one sync per batch, not per block).
- **Indexes** (`ledger_index.dat`, `volumes_idx.dat`, each volume's `idx.dat`) list structure — the
  starting block, checkpoints, volumes, block files — not blocks, so they change only when that
  structure does. Each is replaced durably (written aside, synced, renamed, directory synced). A
  new block file or volume is synced, with its directory entry, before an index lists it; an index
  naming a missing block file is refused at mount (skipping it would shift every later block id).

## Ops

Upgrade requires **re-init** of beacon/relay/miner work directories that still use
the old `workDir/data` tree.

## Node state files

Small files a node keeps beside the ledger for the beacon authority
([BEACON_AUTHORITY.md](BEACON_AUTHORITY.md)). Paths are relative to the role's work directory.
Each is replaced durably (`node_file::replace` → `utl::replaceFile`: written aside, synced, renamed,
directory synced): after a crash or a power loss it holds the old content or the new, never part of
either. `signed-tip.dat` is durable before the tip is served, and the block it signs was committed
before it was signed; `beacon-cert.dat` and `equivocation.dat` right after the node takes their
content (if that write fails, the node logs an error and holds it in memory only). At start a file
that does not parse stops the node; one that parses but no longer applies (a certificate on chain
now or no longer signed, superseded evidence) is removed with a log line. The beacon also refuses a
`signed-tip.dat` of another network.

| File | Roles | Content | Lifetime |
|------|-------|---------|----------|
| `keys/beacon-signing.key`, `.pub` | beacon | The ML-DSA-65 signing key pair, as `pp-client keygen` writes it: private key in hex (raw bytes also read), public key in hex. Both required; they must match | Created by `--init` when both are absent (one alone is refused); certified as generation 1 |
| `keys/beacon-signing.next.key`, `.pub` | beacon | A key pair staged for a rotation, same format; both or neither | Operator-managed |
| `data/signed-tip.dat` | beacon | The latest signed tip: `SignedTip` versioned bytes (version 1) | Rewritten with every block it signs; never signs a different block at or below it. Keep it with the ledger; never copy it between beacons |
| `data/beacon-cert.dat` | beacon, relay, miner | The highest beacon certificate held out of band: the packed `T_BEACON_CERT` record (`binaryPack(Record)`, as `pp-client mk-beacon-cert` writes it) | Removed once the chain commits its generation or a later one, or it no longer verifies |
| `data/rewind.dat` | beacon, relay, miner | A rewind ordered before a restart: `RewindReport` versioned bytes (version 1) | Applied and removed at the next start, before the ledger mounts ([BEACON_AUTHORITY.md § Rewind](BEACON_AUTHORITY.md#rewind)) |
| `data/rewound.dat` | beacon, relay, miner | The last rewind applied, same format (`STATUS.rewound`) | Replaced by the next rewind |
| `data/equivocation.dat` | beacon, relay, miner | The equivocation evidence the node halts on: `Equivocation` versioned bytes (version 1) | Removed once a later generation supersedes it, or it no longer verifies |

A node with no blocks yet keeps `beacon-cert.dat` and `equivocation.dat` and offers them once it
holds its first block. A relay's work directory becomes a beacon's with `pp-beacon --promote` — the
same files, plus the certified key in `keys/beacon-signing.{key,pub}`; the relay's `config.json` is
kept as `config.relay.json` ([BEACON_AUTHORITY.md § Taking over](BEACON_AUTHORITY.md#taking-over)). The beacon also keeps `keys/amp-identity.txt` (its network identity,
[AMP_TRANSPORT.md](AMP_TRANSPORT.md)); relays do too.

