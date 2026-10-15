# Platform integration — ledger roles in pp-node and pp-browser

**Status:** agreed direction, not built. Today pp-ledger runs only as standalone binaries and
pp-browser has no ledger code:
[docs/architecture/PLATFORM_INTEGRATION.md](../../docs/architecture/PLATFORM_INTEGRATION.md).

The original plan carried ledger RPC over libp2p at the pp-browser edge. pp-browser's mesh now runs
on Amp, so everything below is Amp: the same `/pp-ledger/rpc/1.0.0` channel and `binaryPack`
requests as the standalone fleet ([AMP_TRANSPORT.md](../../docs/contracts/AMP_TRANSPORT.md)).

---

## Who runs what

| Role | Standalone pp-ledger | pp-node | pp-browser |
|------|---------------------|---------|------------|
| **Beacon** | yes (`pp-beacon`) | no | no |
| **Relay (ledger gateway)** | yes (`pp-relay`) | **always on** | **off by default**; user may enable |
| **Miner** | yes (`pp-miner`) | no | **opt-in** when configured |
| **Client** | yes (`pp-client`) | no | **always** (UI / local API) |

- **Beacons stay scarce infrastructure**, standalone only.
- **pp-node** is always-on infrastructure, like its circuit and media relays: it would add a ledger
  gateway (the existing `ledger_gateway` capability flag) — an opaque upstream that forwards the
  same RPC bytes toward the beacon. Not a translator.
- **pp-browser** is the pp-node runtime (shared bootstrap, `MeshHost`, identity) plus GUI, client
  and an **optional miner**.

### Relay and miner on pp-browser

- Relay and miner are independent capabilities; both may run in one process.
- **Defaults:** relay off, miner off until configured.
- **A miner never uses the local relay as its upstream**: it dials a pp-node gateway (or another
  explicit upstream). A production guardrail enforces it by default.
- An enabled relay on pp-browser is volunteer infrastructure (intermittent devices make poor
  relays — warn in the UI). pp-node gateways stay on regardless.

---

## Paths

| Path | Transport |
|------|-----------|
| pp-browser UI → local miner / client | In-process (`InProcessLedgerTransport`) |
| pp-browser miner → pp-node gateway | Amp (mesh link) |
| pp-node gateway → upstream relay / beacon | Amp |
| Standalone miner → relay | Amp (today) |

---

## Work in pp-browser

- `cmake/PpLedger.cmake` — consume pp-ledger like the other pp-cpp-* libraries (pinned tag,
  optional source-dir override); widen pp-node's link allowlist deliberately for the gateway.
- A ledger node service under `feature/` (layering per pp-browser `SRC_LAYOUT.md`): config bridge,
  key bridge, lifecycle with `AppRuntime` / `MeshHost`.
- Serve `/pp-ledger/rpc/1.0.0` on the mesh (`MeshHost`) for the gateway role; link the miner and
  client to `ILedgerTransport`.
- Optional ledger routes on pp-node's `StatusHttpServer` instead of a separate `pp-http`.
- Shutdown order when embedded: stop ledger servers (stop → join) → stop `MeshHost` → flush the
  ledger store → drain workers → `AppRuntime` shutdown.

## Work in pp-ledger

- `pp::ledger_runtime` — a target bundling what an embedded node needs, if `pp::server` +
  `pp::client` prove too coarse.
- An `ICryptoProvider` seam for keys held by the host (pp-browser's profile secrets) instead of key
  files.

## Agreed decisions

- No libp2p inside pp-ledger; pp-ledger owns the RPC contract, hosts own sockets.
- Beacon only in standalone pp-ledger.
- PQ crypto (ML-DSA-65) before any stable wire release.
- pp-node = always-on ledger gateway; pp-browser = client + optional miner; relay off by default;
  a miner never defaults to the local relay.
