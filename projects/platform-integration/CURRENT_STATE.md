# Current state — platform integration

| Capability | Status | Notes |
|------------|--------|-------|
| Design | **Decided** | [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) PI001–PI006 (2026-10-19): one ledger role per pp-node on its mesh runtime, the node's identity, the ledger's own work directory, configured upstreams |
| Shared runtime (pi1) | **Yes** | `Server::runOnHost` serves and calls on a host's Amp runtime (`network::HostedAmpRuntime`); the host keeps runtime-wide settings and advertises `AmpLedgerServer::protocolIds()`; no io-lane call reaches a stopped node — normative in [PLATFORM_INTEGRATION.md § Networking](../../docs/architecture/PLATFORM_INTEGRATION.md#networking), [THREADING.md](../../docs/architecture/THREADING.md) |
| Role runner (pi2) | **Yes** | `RoleRunner`: a role on its work directory until stopped, a fresh server after a rewind, on a host's runtime or its own (a beacon standalone only); `stop()` from any thread or a signal; the apps use it — normative in [PLATFORM_INTEGRATION.md § pp-ledger library shape](../../docs/architecture/PLATFORM_INTEGRATION.md#pp-ledger-library-shape) |
| Mesh as a library (pi3) | **Yes** | The mesh is pp-cpp-mesh v1.0.0; pp-node stays in pp-browser. A host advertises the ledger's protocols while `RoleRunner::Options::onListening` says they are bound (`MeshHost::SetAppProtocols`) |
| pp-node ledger role (pi4) | Not started | Next, in pp-browser: `cmake/PpLedger.cmake` on a release with `onListening`, the role in pp-node's config and lifecycle, the two smokes |
