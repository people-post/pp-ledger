# PP platform integration — cross-repo architecture

How pp-ledger relates to the other PP repositories today. Planned integration (ledger roles in
pp-node / pp-browser): [projects/platform-integration](../../projects/platform-integration/DESIGN.md).

| Repo | Role |
|------|------|
| [pp-cpp-common](https://github.com/people-post/pp-cpp-common) | `Value` / `Object` / `FiFoMap`, JSON, `Logger`, `Module`, `Roe`, binary serialization |
| [pp-cpp-crypto](https://github.com/people-post/pp-cpp-crypto) | libsodium, ML-DSA, ML-KEM |
| [pp-cpp-amp](https://github.com/people-post/pp-cpp-amp) | Amp transport: ADP (UDP), MSH handshake, channel mux, link manager |
| **pp-ledger** (this repo) | Standalone ledger: `pp-beacon`, `pp-relay`, `pp-miner`, `pp-client`, optional `pp-http` |
| pp-browser | Desktop / mobile client and `pp-node` (mesh circuit + media relay). **Does not consume pp-ledger** |

---

## Dependencies

| Repo | Depends on |
|------|------------|
| pp-ledger | pp-cpp-amp, pp-cpp-crypto, pp-cpp-common |
| pp-browser | pp-cpp-ui, pp-cpp-amp, pp-cpp-crypto, pp-cpp-common |
| pp-cpp-amp | pp-cpp-crypto, pp-cpp-common |
| pp-cpp-crypto | pp-cpp-common |

pp-ledger includes, in order (`CMakeLists.txt`):

| Module | Pin file | Override | Skipped when |
|--------|----------|----------|--------------|
| pp-cpp-common | `cmake/PpCppCommon.cmake` | `PP_CPP_COMMON_SOURCE_DIR` | `pp_common` already defined |
| pp-cpp-crypto | `cmake/PpCppCrypto.cmake` | `PP_CPP_CRYPTO_SOURCE_DIR` | `pp_crypto` and `sodium` already defined |
| pp-cpp-amp | `cmake/PpCppAmp.cmake` | `PP_CPP_AMP_SOURCE_DIR`; else a sibling `../pp-cpp-amp` when present; else the pinned tag | `pp_amp_l1` already defined |

Each pin is a release tag on the dependency's `main`. pp-browser pins the same libraries in its
own `cmake/`; the two repos may sit on different tags.

---

## pp-ledger library shape

**Targets** (`cmake/PpLedgerTargets.cmake`): `pp::ledger_common`, `pp::consensus`,
`pp::network`, `pp::ledger`, `pp::client`, `pp::chain_types`, `pp::chain`, `pp::operation`,
`pp::server`. `pp::operation` (operation files) needs only `pp::chain_types`, `pp::ledger` and
`pp::ledger_common`: an app that must not reach the network links it alone. `pp_ledger_amp` (static,
no alias) holds the Amp binding and is linked by `pp_client` and `pp_server`.

**Options:** `PP_LEDGER_BUILD_APPS` (ON at top level, OFF when embedded), `PP_LEDGER_BUILD_HTTP`
(OFF), `PP_LEDGER_BUILD_TESTS` (OFF).

**Embedding:** another project takes pp-ledger with `FetchContent` or `add_subdirectory`; paths are
the project's own (`PROJECT_SOURCE_DIR`), and pp-cpp-common / pp-cpp-crypto / googletest already
defined by the parent are reused. Headers include from `src/` (`#include "operation/Operation.h"`).

**`src/lib/common`** (`pp_ledger_common`) keeps only ledger-specific pieces: `Crypto`, `Meta`
(an alias of pp-cpp-common `Object`), `BinaryPack` wrappers, `Service`, `ThreadSafeQueue`,
`Utilities` (ML-DSA helpers, JSON loading). Everything generic comes from pp-cpp-common. Vendored:
CLI11 and cpp-httplib (HTTP only).

**Crypto:** accounts and transactions are signed with **ML-DSA-65** only (`Crypto::TK_ML_DSA_65`).

---

## Networking

All ledger traffic is Amp over UDP ([AMP_TRANSPORT.md](../contracts/AMP_TRANSPORT.md)):

```text
UDP → ADP → MSH (ML-DSA identity) → channel /pp-ledger/rpc/1.0.0 → binaryPack Client::Request
```

- One handler table for every role: `Server::handle` / `handleLater` per request type, served
  by `Server::serveRequest`. Topology rules:
  [LEDGER_TOPOLOGY.md](LEDGER_TOPOLOGY.md).
- Client side: `ILedgerTransport` (unframed request / response bytes) with `AmpLedgerTransport`
  (threaded over an `AmpHost`, or caller-driven for tests).
- **On a host's runtime** (`Server::runOnHost`, `network::HostedAmpRuntime`): a node serves and
  calls on another program's Amp runtime — pp-node's mesh — with its socket, identity and listen
  address; the configured `port` and identity key go unused. The host keeps the runtime-wide
  settings: it drives the runtime, advertises `AmpLedgerServer::protocolIds()` with its own
  protocols, sets listen addresses, link limits and refusing unhandled opens. pp-ledger only binds
  and removes its protocol handlers, and no io-lane call reaches a stopped node
  ([THREADING.md](THREADING.md) rule 6). `InProcessLedgerTransport` exists
  but nothing uses it yet.
- Peers are curated ADP multiaddrs (`/ip4/…/udp/…/adp/1.0.0/p2p/<PeerId>`); there is no DHT or
  open discovery.
- pp-ledger contains no libp2p; `LedgerPeerId` only formats a libp2p-compatible PeerId from an
  ML-DSA-65 public key.

---

## Runtime today

| Role | Where it runs |
|------|---------------|
| Beacon, relay, miner, client | Standalone pp-ledger binaries only |
| HTTP / MCP proxy | `pp-http` (built with `PP_LEDGER_BUILD_HTTP`): loopback by default; talks to one relay (`--beacon`) and one miner (`--miner`) over Amp with an ephemeral identity |

Packaging: one image `ghcr.io/people-post/pp-ledger` with all binaries; `deploy/compose.yml` runs
beacon, relay, miner and http ([deploy/README.md](../../deploy/README.md)). CI builds on
ubuntu-24.04, macos-14 and windows-2022 (`scripts/ci-build.sh`).

**pp-browser / pp-node:** no ledger code. pp-node's link allowlist
(`cmake/PpBrowserNoUiDeps.cmake` in pp-browser) admits no pp-ledger target. The only ledger
touchpoint is the `ledger_gateway` mesh capability flag — directory and config only, unused by
any dial path.
