# Current state — platform integration

| Capability | Status | Notes |
|------------|--------|-------|
| Design | **Decided** | [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) PI001–PI006 (2026-10-19): one ledger role per pp-node on its mesh runtime, the node's identity, the ledger's own work directory, configured upstreams |
| Shared runtime (pi1) | **Yes** | `Server::runOnHost` serves and calls on a host's Amp runtime (`network::HostedAmpRuntime`); the host keeps runtime-wide settings and advertises `AmpLedgerServer::protocolIds()`; no io-lane call reaches a stopped node — normative in [PLATFORM_INTEGRATION.md § Networking](../../docs/architecture/PLATFORM_INTEGRATION.md#networking), [THREADING.md](../../docs/architecture/THREADING.md) |
| Role runner (pi2) | **Yes** | `RoleRunner`: a role on its work directory until stopped, a fresh server after a rewind, on a host's runtime or its own (a beacon standalone only); `stop()` from any thread or a signal; the apps use it — normative in [PLATFORM_INTEGRATION.md § pp-ledger library shape](../../docs/architecture/PLATFORM_INTEGRATION.md#pp-ledger-library-shape). pp-ledger's part is done; next: pi3 in the mesh repo |
| pp-node ledger role | Not started | After pp-node and the mesh code move out of pp-browser (pi3, pi4) |
