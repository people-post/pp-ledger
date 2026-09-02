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
  `cmake/PpCppCommon.cmake`, pin release tag e.g. `v0.2.0`).
- **pp-ledger** is fetched by pp-browser via FetchContent only (`cmake/PpLedger.cmake`,
  planned — git tag pin, no sibling checkout shortcut).
- When pp-ledger is embedded, it must **not** re-fetch crypto if the parent already
  provides `pp_crypto` / `sodium`. Value/Meta/JSON live in **pp-cpp-common**
  (`common/Value.h`, `common/io/Json.h`); nlohmann is not used.
- When pp-ledger is embedded, skip FetchContent for pp-cpp-common if `pp_common`
  already exists (`if(NOT TARGET pp_common)`).

Standalone pp-ledger (Docker, CI, ops) fetches pp-cpp-crypto and pp-cpp-common
via FetchContent (optional `-DPP_CPP_*_SOURCE_DIR=` override only).

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

Each connection carries **one** request/response pair (short-lived TCP). Public-facing
relay/miner servers enforce per-IP caps, connection/RPC rate limits, read timeouts, and
a 512 KiB default payload cap via `SecurityConfig::publicDefaults()` and
`ConnectionGuard`. Trusted beacon servers use higher limits (`trustedDefaults()`).
Handler work runs on a bounded queue + worker pool in `Server`; I/O stays on
`FetchServer`.

`Client::Request` = `{ version, type, payload }` (`src/client/Client.h`). Beacon,
miner, and relay servers implement the same handler surface via
`Server::handleParsedRequest`. **`LedgerFrameCodec`** (`src/network/LedgerFrameCodec.h`)
is the canonical u32 BE length-prefix implementation (max payload 16 MiB).

### Transport layers (landed in pp-ledger)

```text
┌─────────────────────────────────────────────────────────┐
│  Application (pp-ledger): Client::Request / chain / consensus │
└───────────────────────────┬─────────────────────────────┘
                            │  ILedgerTransport (unframed RPC envelope bytes)
         ┌──────────────────┼──────────────────┐
         │                  │                  │
 TcpLedgerTransport   InProcessLedgerTransport   Libp2pLedgerTransport
 (u32 LedgerFrameCodec)  (no framing)           (pp-browser only; u32)
```

| Path | Transport |
|------|-----------|
| UI → local miner (pp-browser, same process) | **In-process** |
| pp-browser miner → **pp-node** relay | **Libp2p** `/pp-ledger/rpc/1.0.0` |
| pp-node relay → upstream relay / beacon | **AMP** (same RPC bytes) |
| Standalone `pp-miner` → `pp-relay` (or beacon) | **AMP** `/pp-ledger/rpc/1.0.0` |
| pp-browser volunteer relay → peers | **Libp2p** |
| pp-browser volunteer relay → upstream | Libp2p and/or AMP to pp-node |

### pp-node as protocol gateway

pp-node is **not** a semantic translator. It forwards the **same RPC bytes** on
different sockets:

```text
  pp-browser miner ──libp2p /pp-ledger/rpc/1.0.0──► pp-node relay
                                                          │
                                                          │ AMP (same RPC)
                                                          ▼
                                                    upstream relay / beacon
```

### Libp2p protocol (planned spec)

- **Protocol ID:** `/pp-ledger/rpc/1.0.0`
- **Framing:** **u32 BE** via `LedgerFrameCodec` (same as TCP). Do not reuse pp-browser
  general `StreamFrameIo` u64 framing for ledger RPC.
- **Handler rule:** hop off libp2p IO thread before blocking work (same as
  `DialBackService` in pp-browser).
- **Discovery:** libp2p on PP nodes; deprecate BitTorrent `DhtRunner` on PP fleet over
  time. Standalone TCP miners may keep DHT until sunset.

### Answer: miner ↔ pp-node

**Yes.** When pp-browser acts as a miner, **cross-machine blockchain RPC with pp-node
uses libp2p**, not TCP. Local UI/client ↔ miner stays in-process.

---

## pp-ledger library shape

### Trim local `src/lib/common`

Fetch **pp-cpp-common**; **remove duplicated modules** already provided there.
**Keep in pp-ledger** (until moved upstream if ever):

| Module | Notes |
|--------|--------|
| Value / Object / Meta / FiFoMap | Human-friendly intermediate document tree (JSON-shaped). `Meta` aliases `Object`. Not a canonical signing/identity type — real structs own comparison. Move to pp-cpp-common later. |
| Crypto | Signing/verify (ML-DSA-65 via pp-cpp-crypto) |
| Service, ThreadSafeQueue | Server threading |
| io/Json | Value/Object ↔ UTF-8 JSON (i64+double numbers; structured errors) |
| Extended Utilities | ML-DSA-65 helpers, binaryPack wrappers (`pp::utl`), `loadJsonFile`, … |

**Status (2026-08-27):** `pp_ledger_common` target (renamed from `pp_lib`); include
pp-cpp-common headers as `common/…` directly. Ledger-only: `Meta.h`, `BinaryPack.hpp`
(`pp::utl` wrappers), Crypto, Service, Utilities.

### Exported CMake targets (namespaced `pp::`) — landed

```text
pp::ledger_common
pp::consensus
pp::network
pp::ledger
pp::client
pp::chain
pp::server          # BeaconServer, MinerServer, RelayServer, Server base
pp::ledger_runtime  # deferred: role-based start/stop for embedders
```

### Build options — landed

```cmake
option(PP_LEDGER_BUILD_APPS "pp-beacon, pp-miner, …" ON)
option(PP_LEDGER_BUILD_TESTS "ctest" OFF)
option(PP_LEDGER_BUILD_HTTP "pp-http" OFF)
```

`-DBUILD_TESTING=ON` / `-DBUILD_HTTP=ON` remain accepted aliases.

**pp-browser embed profile:**

```cmake
PP_LEDGER_BUILD_APPS=OFF
PP_LEDGER_BUILD_TESTS=OFF
PP_LEDGER_BUILD_HTTP=OFF   # integrate routes into StatusHttpServer instead
```

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

- `cmake/PpCppCommon.cmake` — fetches pp-cpp-common, pin `v0.2.0`.
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
- Docker image / CI (`scripts/ci-build.sh`) on **Linux, macOS, and Windows** (`ubuntu-24.04`, `macos-14`, `windows-2022`)
- TCP fetch + DHT for public internet deployment (Winsock on Windows via `src/network/platform/`)

Embedded PP fleet (pp-node / pp-browser) uses libp2p among themselves; standalone
binaries serve migration and ops.

---

## Roadmap (implementation order)

| Phase | Scope |
|-------|--------|
| **1** | pp-ledger: FetchContent pp-cpp-common; trim local common — **landed** |
| **2** | pp-ledger: CMake export (`pp::` targets), options — **landed**; tag `v1.0.0` open |
| **3** | `ILedgerTransport`; TCP + in-process — **landed**; libp2p in pp-browser open |
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
| 2026-08-27 | Meta → Value/Object backbone with FiFoMap insertion order (memory=JSON=binary); explicit Null; JSON i64+double only; human intermediate tree, not canonical identity |
| 2026-08-27 | `ILedgerTransport` + `TcpLedgerTransport` + `InProcessLedgerTransport`; `LedgerFrameCodec` u32 BE on wire only |
| 2026-08-27 | `pp_lib` → `pp_ledger_common`; `pp::` ALIAS targets; `PP_LEDGER_BUILD_*` options |
| 2026-08-27 | FetchContent-only deps (no sibling shortcut); optional `PP_CPP_*_SOURCE_DIR` override |
| 2026-08-27 | In-process transport: no length prefix; libp2p ledger RPC uses u32 BE (not StreamFrameIo u64) |
| 2026-08-28 | Public TCP abuse controls: tiered `SecurityConfig`, `ConnectionGuard`, bounded handler queue + worker pool on standalone servers |
