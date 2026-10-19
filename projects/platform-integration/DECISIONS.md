# Decisions — platform integration

Agreed with the maintainer on 2026-10-19, after pp-browser's mesh moved to Amp (the first plan had
pp-ledger on TCP, then libp2p at the edge).

## PI001 — A ledger role runs on the node's mesh runtime

**Date:** 2026-10-19
**Superseded by:** [PLATFORM_INTEGRATION.md § Networking](../../docs/architecture/PLATFORM_INTEGRATION.md#networking), [THREADING.md](../../docs/architecture/THREADING.md) rule 6 (normative; this entry keeps the why).
**Decision:** In pp-node a ledger role serves and calls on the node's Amp runtime (`MeshHost`'s
`AmpStack`) through `network::HostedAmpRuntime`, not on a socket of its own.
**Rationale:** Both speak Amp now. One runtime is one UDP port to open and one identity to manage,
and ledger traffic gets the mesh's NAT traversal and circuit relays — it fits a beacon hidden behind
relays.
**Cons:** pp-ledger must live by the host's threading and lifetime (PI006); a seam to keep
(`AmpHost`).
**Alternatives:** A second runtime per node (works almost unchanged; a second port and identity,
no mesh reach for the ledger).

## PI002 — The node's device key is its ledger identity

**Date:** 2026-10-19
**Decision:** On a host the ledger role has the host's identity: pp-node's device ML-DSA key and
its peer id. Standalone relays name a pp-node by that peer id in `beacon` / `beacons` and
`allowedPeers`.
**Rationale:** Follows from one runtime: a runtime has one MSH identity.
**Cons:** A node's ledger role and mesh services share a peer id (and its reputation).

## PI003 — One ledger role per pp-node; keys from files

**Date:** 2026-10-19
**Decision:** A pp-node runs a relay or a miner, chosen in its config — not both. A miner reads its
account keys from files in its ledger work directory, as `pp-miner` does.
**Rationale:** Simple first; both roles in one process may come later, without spending effort on
it now. Files keep pp-node's miner the same as pp-miner (same docs, same tools).
**Alternatives:** Both roles at once (the earlier pp-browser plan); host-held keys
(`ICryptoProvider`, left for pp-browser profiles).

## PI004 — The ledger keeps its own work directory inside the node's

**Date:** 2026-10-19
**Decision:** The role's work directory sits inside the node's data directory and holds pp-ledger's
own `config.json`, keys and data. pp-node's config only chooses the role and points at it.
**Rationale:** pp-ledger stays self-describing: the same files, configuration docs and tools as a
standalone node; nothing to translate.
**Alternatives:** Folding ledger settings into pp-node's config.

## PI005 — Upstreams are configured; discovery later

**Date:** 2026-10-19
**Decision:** A pp-node role's upstreams are configured multiaddrs, as for standalone nodes. Finding
pp-node relays through the mesh directory (`ledger_gateway`) comes later.

## PI006 — On a host, pp-ledger is a guest

**Date:** 2026-10-19
**Superseded by:** [THREADING.md](../../docs/architecture/THREADING.md) rule 6, [AMP_TRANSPORT.md § Modules](../../docs/contracts/AMP_TRANSPORT.md#modules-pp-ledger) (normative; this entry keeps the why).
**Decision:** On a host's runtime pp-ledger never drives it and sets no runtime-wide setting
(advertisement, listen addresses, link limits, refusing unhandled opens): it posts work and binds
and removes its own protocol handlers; the host advertises `AmpLedgerServer::protocolIds()`. Every
call from the io lane into pp-ledger goes through `AmpHost::guard`, so once a node stops none runs.
**Rationale:**
- **The host's runtime outlives the node.** Standalone, a node's runtime stops and joins its pump
  before anything is destroyed; on a host it runs on, so queued work, open channels and late
  replies would reach a destroyed server.
- **Shared settings clobber each other.** pp-node's `MeshHost` replaces the whole advertised list,
  and pp-ledger's own advertising appended to it and never withdrew.
**Cons:** A post from another thread waits for the host's current turn (posted work runs under the
host's io lock); a node's close waits for it too.
