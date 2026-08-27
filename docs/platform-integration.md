# PP platform integration — cross-repo architecture

This document captures **agreed architecture and migration decisions** for integrating
**pp-ledger**, **pp-cpp-common**, **pp-browser**, and **pp-node**. It lives in
pp-ledger because that repo is the blockchain library source of truth; mirror or link
from sibling repos when convenient.

**Status:** agreed design (2026-08-26). Implementation is phased; see [Roadmap](#roadmap).

**Related repos:**

| Repo | Role |
|------|------|
| [people-post/pp-cpp-common](https://github.com/people-post/pp-cpp-common) | Shared C++ foundation (`pp_common`): Logger, Module, Roe, Serialize, WorkerPool, … |
| [people-post/pp-ledger](https://github.com/people-post/pp-ledger) (this repo) | Blockchain library + standalone beacon/relay/miner/client binaries |
| [people-post/pp-browser](https://github.com/people-post/pp-browser) | GUI app; enhanced pp-node runtime + optional miner |
| pp-node | Headless binary inside pp-browser (`src/app/node/`) — infrastructure relay |

---

## Dependency pyramid

```text
                    pp-browser (GUI)
                   /       |         \
                  /        |          \
         pp-cpp-common    |      libp2p / GUI stack
                          |
                      pp-ledger  ← FetchContent library
                          |
                   pp-cpp-common
```

- **pp-cpp-common** is fetched once at the **top-level consumer** (pp-browser root
  `cmake/PpCppCommon.cmake`, pin release tag e.g. `v0.1.0`).
- **pp-ledger** is fetched by pp-browser after json/sodium deps exist
  (`cmake/PpLedger.cmake`, planned).
- When pp-ledger is embedded, it must **not** re-vendor json or re-fetch crypto if the
  parent already provides `nlohmann_json::nlohmann_json` and `pp_crypto` / `sodium`.
- When pp-ledger is embedded, skip FetchContent for pp-cpp-common if `pp_common`
  already exists (`if(NOT TARGET pp_common)`).

Standalone pp-ledger (Docker, CI, ops) keeps vendored json and fetches pp-cpp-crypto
(and may FetchContent pp-cpp-common on its own).

---

## Node roles (agreed)

### Who runs what

| Role | Standalone pp-ledger | pp-node | pp-browser |
|------|---------------------|---------|------------|
| **Beacon** | yes (`pp-beacon`) | no | no |
| **Relay** | yes (`pp-relay`) | **always on** (primary) | **off by default**; user may enable |
| **Miner** | yes (`pp-miner`) | no | **opt-in** when configured |
| **Client** | yes (`pp-client`) | no | **always** (UI / local API) |

Beacons remain **scarce infrastructure** (see `README.md` / design docs). Edge
participants are pp-node (relay infra) and pp-browser (client + optional miner).

### pp-node vs pp-browser

- **pp-node** = always-on **infrastructure relay**, analogous to circuit relay and
  media relay: libp2p host + blockchain `RelayServer` + existing relay services.
- **pp-browser** = **enhanced pp-node runtime** (shared bootstrap / MeshHost / identity)
  plus GUI, client, and **optional miner**. It is not “just a thin client.”

### Relay and miner on pp-browser (agreed policy)

- **Relay and miner are independent capabilities** — both may run in the same process
  if the user enables them.
- **Defaults:** relay **off**, miner **off** until configured.
- **Routing rule:** when mining, the miner **does not use the local relay**. Upstream
  is **pp-node relay over libp2p** (or another explicit upstream), not loopback.
- Config guardrail: `miner.avoid_local_relay: true` by default in production.
- If the user enables relay on pp-browser, treat it as **volunteer infrastructure**
  (intermittent devices make poor relays — UI warning recommended).

pp-node relay stays **on** regardless of whether any pp-browser instance is mining.

---

## Networking: TCP vs libp2p

### Principle

**Do not embed libp2p inside pp-ledger core.** pp-ledger owns the **RPC contract**;
transports are pluggable.

Today’s wire contract (standalone):

```text
TCP connect → u32 BE length + Client::Request → u32 BE length + response → close
```

`Client::Request` = `{ version, type, payload }` (`src/client/Client.h`). Beacon,
miner, and relay servers implement the same handler surface via
`Server::handleParsedRequest`.

### Transport layers

```text
┌─────────────────────────────────────────────────────────┐
│  Application (pp-ledger): Client::Request / chain / consensus │
└───────────────────────────┬─────────────────────────────┘
                            │  ILedgerTransport (planned)
         ┌──────────────────┼──────────────────┐
         │                  │                  │
   TcpTransport      InProcessTransport    Libp2pTransport
   (pp-ledger)       (same process)        (pp-browser only)
```

| Path | Transport |
|------|-----------|
| UI → local miner (pp-browser, same process) | **In-process** |
| pp-browser miner → **pp-node** relay | **Libp2p** `/pp-ledger/rpc/1.0.0` |
| pp-node relay → internet upstream relay/beacon | **TCP** (existing fetch protocol) |
| Standalone `pp-miner` → `pp-relay` | **TCP** (until fleet migrates) |
| pp-browser volunteer relay → peers | **Libp2p** |
| pp-browser volunteer relay → upstream | **TCP** and/or libp2p to pp-node |

### pp-node as protocol gateway

pp-node is **not** a semantic translator. It forwards the **same RPC bytes** on
different sockets:

```text
  pp-browser miner ──libp2p /pp-ledger/rpc/1.0.0──► pp-node relay
                                                          │
                                                          │ TCP fetch
                                                          ▼
                                                    upstream relay / beacon
```

### Libp2p protocol (planned spec)

- **Protocol ID:** `/pp-ledger/rpc/1.0.0`
- **Framing:** match fetch protocol (`u32 BE` length + payload) or align with
  pp-browser stream conventions (`u64 BE` + payload) — **pick one before v1.0 tag**.
- **Handler rule:** hop off libp2p IO thread before blocking work (same as
  `DialBackService` in pp-browser).
- **Discovery:** libp2p on PP nodes; deprecate BitTorrent `DhtRunner` on PP fleet over
  time. Standalone TCP miners may keep DHT until sunset.

### Answer: miner ↔ pp-node

**Yes.** When pp-browser acts as a miner, **cross-machine blockchain RPC with pp-node
uses libp2p**, not TCP. Local UI/client ↔ miner stays in-process.

---

## pp-ledger library shape (planned)

### Trim local `src/lib/common`

Fetch **pp-cpp-common**; **remove duplicated modules** already provided there.
**Keep in pp-ledger** (until moved upstream if ever):

| Module | Notes |
|--------|--------|
| Meta | Ledger/client/server wire types |
| Crypto | Signing/verify (ML-DSA-65 via pp-cpp-crypto) |
| Service, ThreadSafeQueue | Server threading |
| io/Json | Meta ↔ JSON for HTTP/CLI |
| Extended Utilities | ML-DSA-65 helpers, binaryPack wrappers (`pp::utl`), `loadJsonFile`, … |

**Status (2026-08-27):** `cmake/PpCppCommon.cmake` lands; local Logger / Module /
ResultOrError / Serialize / Error are shims or deleted in favor of `pp_common`.
`BinaryPack.hpp` keeps `pp::utl::binaryPack/Unpack` as thin wrappers over `pp::`.
Rename `pp_lib` → `pp_ledger_common` still planned.

### Exported CMake targets (namespaced `pp::`)

```text
pp::ledger_common
pp::consensus
pp::network
pp::ledger
pp::client
pp::chain
pp::server          # BeaconServer, MinerServer, RelayServer, Server base
pp::ledger_runtime  # planned: role-based start/stop for embedders
```

Convenience interface (optional):

```cmake
# pp_ledger_node — chain + server stack without apps
```

### Build options (planned)

```cmake
option(PP_LEDGER_BUILD_APPS "pp-beacon, pp-miner, …" ON)
option(PP_LEDGER_BUILD_SERVER "pp_server" ON)
option(PP_LEDGER_BUILD_TESTS "ctest" OFF)
option(PP_LEDGER_USE_VENDORED_DEPS "vendor json; crypto via pp-cpp-crypto" ON)
option(PP_LEDGER_BUILD_HTTP "pp-http" OFF)
```

**pp-browser embed profile:**

```cmake
PP_LEDGER_BUILD_APPS=OFF
PP_LEDGER_BUILD_TESTS=OFF
PP_LEDGER_USE_VENDORED_DEPS=OFF
PP_LEDGER_BUILD_HTTP=OFF   # integrate routes into StatusHttpServer instead
```

Standalone ops/Docker keeps apps ON and vendored deps ON.

---

## Post-quantum crypto (before release)

Both pp-ledger and pp-browser use the **same PQ stack** via
[pp-cpp-crypto](https://github.com/people-post/pp-cpp-crypto) (`pp_crypto`, ML-DSA-65 /
ML-KEM-768 + libsodium).

**pp-ledger status (PQ-only, pre-release):** account/tx signing is **ML-DSA-65 only**
(`Crypto::TK_ML_DSA_65`). Classical Ed25519 support was removed (no product release yet;
no wire version bump).

Remaining:

1. Optional `ICryptoProvider` if embedders need injection; `keyType` remains on wallets.
2. Migrate Ouroboros VRF / slot leader logic explicitly (hybrid or PQ-VRF — TBD).
3. **Stabilize libp2p RPC and ledger wire formats** now that signature/key sizes are PQ.
4. pp-browser: ledger keys live in `ProfileSecretsService`; avoid long-term plaintext
   `key.txt` files on disk.

---

## Configuration sketch (pp-browser / pp-node)

```json
{
  "ledger": {
    "miner": {
      "enabled": false,
      "upstream": {
        "type": "libp2p",
        "peer": "<pp-node-peer-id>"
      },
      "avoid_local_relay": true
    },
    "relay": {
      "enabled": false,
      "upstream": {
        "type": "tcp",
        "host": "upstream.example.com",
        "port": 8519
      }
    }
  }
}
```

**pp-node profile:** `relay.enabled=true` always (blockchain + existing circuit/media
relay). Miner block omitted or `enabled=false`.

**pp-browser defaults:** both `miner.enabled` and `relay.enabled` false; user opts in.

Legacy standalone miners continue to use TCP `beacons[]` / `host:port` in
`config.json` until migrated.

---

## pp-browser integration notes (current state)

As of **pp-browser `develop`** (FetchContent migration merged):

- `cmake/PpCppCommon.cmake` — fetches pp-cpp-common, pin `v0.1.0`.
- Duplicated `src/common/*.cpp` removed; `src/common/PbrCompat.h` force-included on
  `pp_common` for `pbr::` aliases.
- Root order: `PpCppCommon` → `dependencies` (json, sodium, …) → `add_subdirectory(src)`.

**Planned next in pp-browser:**

- `cmake/PpLedger.cmake` — same pin-tag pattern, after dependencies.
- `src/base/ledger/` — `LedgerNodeService`, config bridge, key bridge, lifecycle with
  `AppRuntime` / `MeshHost`.
- Libp2p handler `/pp-ledger/rpc/1.0.0` on `MeshHost`.
- Link miner/client to `ILedgerTransport`; libp2p for pp-node upstream, in-process for
  local UI.

**Shutdown order (embedded):**

```text
1. Stop ledger servers (miner/relay setStop → join)
2. Stop MeshHost / libp2p
3. Flush ledger disk
4. WorkerPool drain
5. AppRuntime shutdown
```

---

## Standalone pp-ledger (unchanged purpose)

Continue to ship and support:

- `pp-beacon`, `pp-relay`, `pp-miner`, `pp-client`, `pp-http`
- Docker image / CI (`scripts/ci-build.sh`)
- TCP fetch + optional DHT for public internet deployment

Embedded PP fleet (pp-node / pp-browser) uses libp2p among themselves; standalone
binaries serve migration and ops.

---

## Roadmap (implementation order)

| Phase | Scope |
|-------|--------|
| **1** | pp-ledger: FetchContent pp-cpp-common; trim local common — **landed** (shims + keep Meta/Crypto/Service/…); rename `pp_lib` → `pp_ledger_common` still open |
| **2** | pp-ledger: CMake export (`pp::` targets), options, tag `v1.0.0` library release |
| **3** | Extract `ILedgerTransport`; in-process + TCP implementations; standalone green |
| **4** | PQ crypto (ML-DSA-65 signing via pp-cpp-crypto) — **landed** PQ-only; then stabilize wire |
| **5** | pp-browser: `PpLedger.cmake`, `src/base/ledger/`, embed miner/relay roles |
| **6** | Libp2p `/pp-ledger/rpc/1.0.0` on pp-node + pp-browser; miner → pp-node over libp2p |
| **7** | Optional block gossip stream; deprecate DHT on PP nodes |

---

## Copy-paste pointers for sibling repos

When editing other repos, add a one-line link at the top of the relevant doc:

```markdown
Cross-repo architecture: [pp-ledger docs/platform-integration.md](https://github.com/people-post/pp-ledger/blob/develop/docs/platform-integration.md)
```

Suggested locations:

| Repo | File |
|------|------|
| pp-cpp-common | `README.md` — “Consumers” section |
| pp-browser | `docs/README.md` or `docs/architecture/PLATFORM.md` |
| pp-ledger | `AGENTS.md`, this file |

---

## Decision log

| Date | Decision |
|------|----------|
| 2026-08-26 | pp-ledger becomes FetchContent library; local common trimmed, not deleted wholesale |
| 2026-08-26 | pp-cpp-common fetched at consumer; pp-ledger uses `if(NOT TARGET pp_common)` |
| 2026-08-26 | No libp2p inside pp-ledger; libp2p transport only in pp-browser/pp-node |
| 2026-08-26 | pp-node = always-on relay infra; pp-browser = client + optional miner |
| 2026-08-26 | pp-browser relay off by default; user may enable; miner never defaults to local relay |
| 2026-08-26 | Miner ↔ pp-node blockchain RPC over libp2p |
| 2026-08-26 | PQ crypto before stable wire/protocol release |
| 2026-08-26 | Beacon only in standalone pp-ledger; not embedded in pp-node/pp-browser |
| 2026-08-27 | Consume pp-cpp-crypto; account/tx signing is ML-DSA-65 only (drop Ed25519; no version bump pre-release) |
| 2026-08-27 | Consume pp-cpp-common; replace duplicated Logger/Module/Roe/Serialize; keep ledger Meta/Crypto/Service/Utilities |
