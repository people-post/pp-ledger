# Platform integration — ledger roles in pp-node and pp-browser

**Status:** decided (2026-10-19, [DECISIONS.md](DECISIONS.md)); pi1 built. Progress:
[CURRENT_STATE.md](CURRENT_STATE.md); order: [PHASES.md](PHASES.md). Today pp-ledger runs only as
standalone binaries and pp-browser has no ledger code:
[docs/architecture/PLATFORM_INTEGRATION.md](../../docs/architecture/PLATFORM_INTEGRATION.md).

The first plan carried ledger RPC over TCP, then over libp2p at the pp-browser edge. Both sides now
speak Amp — pp-ledger's `/pp-ledger/rpc/1.0.0` channels and pp-browser's mesh run on the same
pp-cpp-amp — so a ledger role in pp-node needs no socket of its own: it runs on the node's mesh
runtime ([PI001](DECISIONS.md#pi001--a-ledger-role-runs-on-the-nodes-mesh-runtime)).

---

## Who runs what

| Role | Standalone pp-ledger | pp-node | pp-browser (later) |
|------|---------------------|---------|--------------------|
| **Beacon** | yes (`pp-beacon`) | no | no |
| **Relay** | yes (`pp-relay`) | **one role per node**, chosen in its config | off by default; user may enable |
| **Miner** | yes (`pp-miner`) | **one role per node**, chosen in its config | opt-in when configured |
| **Client** | yes (`pp-client`) | no | always (UI / local API) |

- **Beacons stay scarce infrastructure**, standalone only (hidden behind relays).
- **pp-node runs a relay or a miner, not both** ([PI003](DECISIONS.md#pi003--one-ledger-role-per-pp-node-keys-from-files)):
  a relay is an opaque upstream that forwards the same RPC bytes toward the beacon (the
  `ledger_gateway` capability); a miner produces blocks with its account keys. Both in one node
  may come later; nothing here should make it harder, nor spend effort on it now.
- **pp-browser** (after pp-node): client, an optional miner, an optional relay. A miner there never
  uses its own relay as its upstream.

---

## One runtime, one identity

```text
                     pp-node process
  ┌───────────────────────────────────────────────────────┐
  │ MeshHost: AmpStack (UDP socket, device ML-DSA key)     │
  │   ├─ mesh L4: DHT, directory, circuit / media relay    │
  │   └─ /pp-ledger/rpc, /pp-ledger/rpc-bulk  ◄── pp-ledger │
  │        RelayServer or MinerServer (Server::runOnHost)  │
  └───────────────────────────────────────────────────────┘
```

- The ledger role serves and calls on the node's runtime (`network::HostedAmpRuntime`): one UDP
  port, the node's NAT traversal and relays for ledger traffic too.
- **Identity:** the node's device key is its ledger peer id
  ([PI002](DECISIONS.md#pi002--the-nodes-device-key-is-its-ledger-identity)). Standalone relays name
  a pp-node in `beacon` / `beacons` and `allowedPeers` by that peer id.
- **The host keeps the runtime** ([PI006](DECISIONS.md#pi006--on-a-host-pp-ledger-is-a-guest)): it
  drives it, advertises `AmpLedgerServer::protocolIds()` with its own protocols, and owns listen
  addresses, link limits and refusing unhandled opens. pp-ledger binds and removes its protocol
  handlers only, and once its node stops no call from the io lane reaches it.

## Paths

| Path | Transport |
|------|-----------|
| pp-node relay / miner ↔ its upstreams (standalone relays, the beacon's relays, other pp-nodes) | Amp, on the node's mesh runtime |
| standalone miner / relay → pp-node relay | Amp |
| pp-browser UI → local miner / client (later) | In-process (`InProcessLedgerTransport`) |

Upstreams are configured multiaddrs, as for standalone nodes; finding pp-node relays through the
mesh directory (`ledger_gateway`) comes later ([PI005](DECISIONS.md#pi005--upstreams-are-configured-discovery-later)).

---

## Work in pp-ledger

- **pi1 — shared runtime** (built): `network::AmpHost` (post, io-thread check, in-flight limits,
  guarded io-lane calls) with `LedgerAmpRuntime` (standalone) and `HostedAmpRuntime` (a host's);
  `Server::runOnHost`; clients attach to either.
- **pi2 — a role runner:** what each app's `main()` does today — load the role from its work
  directory, run it, run a fresh server after a rewind (B022), stop on request — as a library
  entry pp-node calls on its own thread, with `runOnHost`.

## Work in the mesh repo / pp-node

pp-node and the mesh code move out of pp-browser first ([pp-browser order](#order)); then:

- **MeshHost:** a hook for protocols to advertise beside its own (it rebuilds the advertised list),
  and the ledger handlers registered before peers arrive (it refuses unhandled opens). One driver:
  pp-node's main loop also calls `MeshHost::Tick()` while the pump thread drives.
- **pp-cpp-amp:** pp-node on pp-ledger's pin or later (pp-ledger is on v2.21.0).
- **The ledger role** (`feature/`): `cmake/PpLedger.cmake` (pinned tag, source-dir override; the
  link allowlist widened for it); config — the role and a ledger work directory inside the node's
  data directory, holding pp-ledger's own `config.json`, account keys (miner) and data
  ([PI004](DECISIONS.md#pi004--the-ledger-keeps-its-own-work-directory-inside-the-nodes)); lifecycle —
  run the role (pi2) with `runOnHost({&mesh.Amp()->Runtime(), listen})`; shutdown: stop the role
  (stop → join) before `MeshHost::Stop`.
- **Smoke:** a standalone beacon and miner with a pp-node relay between them; a pp-node miner
  behind a standalone relay.

## Order

1. pi1 here (built), pi2 here — no pp-browser change needed.
2. In pp-browser: untangle the mesh code (it reaches into `foundation/runtime`, `foundation/data`,
   crypto and identity, and `domain/net`), then move it with pp-node to the new repo.
3. In the new repo: the MeshHost hook, the single driver, the pp-cpp-amp pin, then the ledger role.
4. Later: discovery through the directory, both roles in one node, pp-browser's client and miner,
   host-held keys (`ICryptoProvider`) for pp-browser profiles.

## Agreed decisions

- No libp2p inside pp-ledger; pp-ledger owns the RPC contract, hosts own sockets.
- Beacon only in standalone pp-ledger.
- PQ crypto (ML-DSA-65) before any stable wire release.
- pp-node: one ledger role (relay or miner) on its mesh runtime; pp-browser: client and optional
  miner later.
