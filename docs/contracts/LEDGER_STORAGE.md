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
volume is mutated on append; rewind may truncate the tip and delete later
volumes.

Default Ledger caps: 64 files × 128 MiB per volume (~8 GiB), up to 10 000 volumes.

## Ops

Upgrade requires **re-init** of beacon/relay/miner work directories that still use
the old `workDir/data` tree.

## Node state files

Small files a node keeps beside the ledger for the beacon authority
([BEACON_AUTHORITY.md](BEACON_AUTHORITY.md)). Paths are relative to the role's work directory.
Each is replaced by writing it aside and renaming it over (`node_file::replace`): a process crash
leaves the old content or the new, never part of one. Nothing is synced to the device, so a power
loss may leave an empty or torn file (it then stops the node at start). `signed-tip.dat` is written
before the tip is served; `beacon-cert.dat` and `equivocation.dat` right after the node takes their
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
| `data/equivocation.dat` | beacon, relay, miner | The equivocation evidence the node halts on: `Equivocation` versioned bytes (version 1) | Removed once a later generation supersedes it, or it no longer verifies |

A node with no blocks yet keeps `beacon-cert.dat` and `equivocation.dat` and offers them once it
holds its first block. The beacon also keeps `keys/amp-identity.txt` (its network identity,
[AMP_TRANSPORT.md](AMP_TRANSPORT.md)); relays do too.

