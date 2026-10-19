# Phases — platform integration

Order only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

- [x] **pi1** Shared runtime (pp-ledger): `network::AmpHost` with `LedgerAmpRuntime` (standalone)
      and `HostedAmpRuntime` (a host's); `Server::runOnHost` — the node takes the host's identity
      and address, asks no identity key, sets no runtime-wide setting; io-lane calls guarded so a
      stopped node is never called; clients attach to either (PI001, PI002, PI006)
- [x] **pi2** Role runner (pp-ledger): `RoleRunner` runs a beacon, relay or miner on its work
      directory — the restart loop after a rewind (B022), stop on request — on a host's runtime or
      its own; the apps use it. On the way: a server's stop source (`Service::setStopSource`), so a
      stop between two servers is not lost; smoke scripts no longer exit silently on an empty grep
- [x] **pi3** The mesh as a library: pp-browser's mesh moved to pp-cpp-mesh (v1.0.0; pp-node stays
      in pp-browser, an app on it). `MeshHost::SetAppProtocols` advertises a hosted ledger's
      protocols beside the mesh's own; the role tells its host when its handlers are bound and before
      they go (`RoleRunner::Options::onListening`, `Server::setOnListening`), so the host advertises
      them only while they are served (the mesh refuses unhandled opens). One driver (MeshPump claims
      the runtime) and pp-cpp-amp v2.24.0 on both sides; pp-cpp-common v1.2.0 here as in pp-browser
- [ ] **pi4** pp-node ledger role (in pp-browser): `cmake/PpLedger.cmake`; config (role, ledger work
      directory); run it on the mesh runtime, advertising on `onListening`; shutdown before
      `MeshHost::Stop`; smoke: a pp-node relay between a standalone beacon and miner, a pp-node miner
      behind a standalone relay (PI003, PI004)
- [ ] **pi5** Later: pp-node relays found through the mesh directory (`ledger_gateway`, PI005);
      both roles in one node; pp-browser client and optional miner; host-held keys
