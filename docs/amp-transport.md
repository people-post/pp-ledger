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

## Dependency

pp-cpp-amp is required via CMake FetchContent (`cmake/PpCppAmp.cmake`), pinned to tag **v0.1.2** (includes ADP keepalive).

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
    "/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon-peer-id>"
  ]
}
```

Legacy beacon objects are accepted: `{ "host", "port", "peerId" }`.

**Relay** (default port 8519):

```json
{
  "port": 8519,
  "keys": ["keys/amp-identity.txt"],
  "beacon": "/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon-peer-id>"
}
```

On startup, each server logs its listen multiaddr. Copy the beacon multiaddr into miner/relay configs.

## Client (`pp-client`)

Pass the target ADP multiaddr as `-H` / `--host`:

```bash
pp-client -b -H '/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/...' status
```

## Related

- pp-cpp-amp [KEEPALIVE.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/KEEPALIVE.md) — NAT link maintenance (v0.1.2+)
- Fleet integration (pp-browser / pp-node): deferred to WS3+
