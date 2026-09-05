# AMP ledger transport (pp-ledger)

**Status:** v2 (2026-09-01) — AMP-only fleet transport.

Cross-repo context: [platform-integration.md](platform-integration.md).

## Planes

| Plane | Role |
|-------|------|
| **AMP (UDP)** | Sole fleet transport for `/pp-ledger/rpc/1.0.0` |
| **HTTP** | Degraded client path via org relay (pp-browser; not in this module) |

TCP fetch (`FetchServer` / `TcpLedgerTransport`) and BitTorrent DHT (`DhtRunner`) are **retired**.

## Protocol

| Field | Value |
|-------|-------|
| `protocol_id` | `/pp-ledger/rpc/1.0.0` |
| L3 policy | Control, `read_once=true`, 8 s read timeout |
| Payload | Unframed `binaryPack(Client::Request/Response)` — **no** u32 `LedgerFrameCodec` |
| Max size | 512 KiB (`ledger::rpc::kMaxPayloadBytes`) |

## Topology (uniform upstream)

Beacon, relay, and miner expose the **same** `/pp-ledger/rpc/1.0.0` handler surface. Downstream
participants do **not** distinguish relay from beacon on the wire — config fields such as
`beacons[]` (miner) and `beacon` (relay) name **opaque upstream endpoints** only.

- **Gateways** (relay, edge relay, pp-node): cache chain data, forward mutating RPCs upstream,
  and may chain (`relay → relay → …`).
- **Terminal beacon**: the only node that **realizes** authoritative mutations (e.g. commits
  `BLOCK_ADD` to the canonical chain). A gateway must not return success on a mutating RPC unless
  its upstream path succeeded (write-through).
- **Reads** (`BLOCK_GET`, `ACCOUNT_GET`, …): gateways may serve from a local replica and
  backfill from upstream on miss.

Ops chooses what sits behind each multiaddr; miners and clients validate **chain state**
(crypto, checkpoints, consistency across multiple upstreams), not peer role.

Full design guidance: [ledger-topology.md](ledger-topology.md).

## Dependency

pp-cpp-amp is required via CMake FetchContent (`cmake/PpCppAmp.cmake`), pinned to tag **v0.1.6**
(ADP keepalive, circuit-carrier default, `MemoryDatagramIo` reorder window). Align with pp-browser’s Amp pin when sharing a sibling checkout.

**Ownership:** Amp supplies L1–L3 transport only (`LedgerRpcChannelPolicy` lives in this repo). Chain tip/range sync, checkpoints, and RPC codecs stay in pp-ledger — do not push ledger sync or BitTorrent-style piece swarm into Amp. See [pp-cpp-amp OWNERSHIP](https://github.com/people-post/pp-cpp-amp/blob/develop/docs/OWNERSHIP.md).

## Modules (pp-ledger)

| File | Role |
|------|------|
| `network/LedgerRpcProtocol.h` | Protocol id, channel policy factory, limits |
| `network/AmpLedgerServer.*` | Bind handler on `PeerLinkManager` |
| `client/AmpLedgerTransport.*` | `ILedgerTransport` over AMP |
| `network/LedgerAmpRuntime.*` | Slim `AmpStack` for standalone binaries |
| `network/ServerAmpSupport.*` | `Server::startAmpServer` glue |
| `network/amp/AmpIdentity.*` | Identity + multiaddr config helpers |
| `network/amp/LedgerPeerId.*` | Fleet-compatible PeerId from ML-DSA-65 |

## Server config (AMP)

**Beacon** (`config.json`):

```json
{
  "host": "0.0.0.0",
  "port": 8517,
  "ampKey": "keys/amp-identity.txt",
  "networkId": "pp-testnet-1",
  "whitelist": []
}
```

`port` is the **UDP** listen port. `amp-identity.txt` is created on `pp-beacon --init`.

**Miner**:

```json
{
  "minerId": 1,
  "keys": ["key.txt"],
  "host": "0.0.0.0",
  "port": 8518,
  "beacons": [
    "/ip4/127.0.0.1/udp/8519/adp/1.0.0/p2p/<upstream-peer-id>"
  ]
}
```

`beacons[]` lists one or more upstream ledger endpoints (relay or terminal beacon). Legacy beacon objects are accepted: `{ "host", "port", "peerId" }`.

Optional network anchor (see [ledger-topology.md](ledger-topology.md#8-network-anchor-participant-config)):

```json
"networkAnchor": {
  "networkId": "pp-testnet-1",
  "genesisHash": "<hex>",
  "trustedCheckpointId": 0
}
```

**Relay** (default port 8519):

```json
{
  "port": 8519,
  "keys": ["keys/amp-identity.txt"],
  "beacon": "/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<upstream-peer-id>"
}
```

Relay `beacon` is a single **upstream** endpoint (terminal beacon or another relay).

On startup, each server logs its listen multiaddr. Copy that multiaddr into downstream
`beacons[]` / relay `beacon` configs as needed.

## Client (`pp-client`)

Pass the target ADP multiaddr as `-H` / `--host`:

```bash
pp-client -b -H '/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/...' status
```

## Related

- [ledger-topology.md](ledger-topology.md) — long-term network design (uniform upstream, realization, sync)
- pp-cpp-amp [KEEPALIVE.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/KEEPALIVE.md) — NAT link maintenance (v0.1.2+)
- pp-cpp-amp `MemoryDatagramIo::SetReorderWindow` — in-process datagram reorder (v0.1.3+)
- Fleet integration (pp-browser / pp-node): deferred to WS3+
