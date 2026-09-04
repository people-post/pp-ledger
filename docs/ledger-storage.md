# Ledger block storage

On-disk layout for the append-only block log (hard cut; no migration from the
former recursive `DirDirStore` / `data/` layout).

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
