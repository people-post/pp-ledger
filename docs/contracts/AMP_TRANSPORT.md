# AMP ledger transport (pp-ledger)

AMP is the only fleet transport.

Cross-repo context: [platform-integration.md](../architecture/PLATFORM_INTEGRATION.md).

## Planes

| Plane | Role |
|-------|------|
| **AMP (UDP)** | Sole fleet transport for `/pp-ledger/rpc/1.0.0` |
| **HTTP** | Degraded client path via org relay (pp-browser; not in this module) |

There is no other transport (no TCP, no DHT).

## Protocol

| Field | Value |
|-------|-------|
| `protocol_id` | `/pp-ledger/rpc/1.0.0` |
| L3 policy | Bulk (`MakeBulkChannelPolicy`), `read_once=true`; `read_timeout` = the call's deadline on the client, 2T on the server ([CONFIGURATION.md](../ops/CONFIGURATION.md#network-timing-fixed)) |
| Payload | Unframed `binaryPack(Client::Request/Response)` — no length prefix |
| Max size | 4 MiB + 64 KiB (`ledger::rpc::kMaxMessageBytes`, Amp's Bulk message): a block of the largest `maxBlockBytes` a chain may set (4 MiB) plus its envelope. The chain sets block sizes; the transport carries them ([WIRE_SCHEMA.md § Block size](WIRE_SCHEMA.md#block-size)) |
| Requests | `BLOCK_ADD` and `TX_ADD` (a block, a record) up to the max size; every other type at most 64 KiB (`kMaxSmallRequestBytes`), refused before it is queued |
| Per peer | At most 16 requests arriving at once (channel opened, body not complete; `kMaxReceivingPerPeer`): one more is reset, so a peer cannot make a node buffer more than that many messages |
| Replies | One the max size cannot carry is replaced by an error reply (not a silent timeout). `TX_PULL` returns at most a block's worth of bytes (`maxBlockBytes`), `TX_GET_BY_WALLET` pages whole blocks within it |

## Topology (uniform upstream)

Beacon, relay, and miner expose the **same** `/pp-ledger/rpc/1.0.0` handler surface. Downstream
participants do **not** distinguish relay from beacon on the wire — config fields such as
`beacons[]` (miner) and `beacon` (relay) name **opaque upstream endpoints** only.

- **Gateways** (relay, edge relay, pp-node): cache chain data, forward mutating RPCs upstream,
  and may chain (`relay → relay → …`).
- **Terminal beacon**: the only node that **realizes** authoritative mutations (e.g. commits
  `BLOCK_ADD` to the canonical chain). A gateway must not return success on a mutating RPC unless
  its upstream path succeeded (write-through).
- **Reads** (`BLOCK_GET`, `ACCOUNT_GET`, …): gateways serve from their own replica.

Ops chooses what sits behind each multiaddr; miners and clients validate **chain state**
(validation at every node, the network anchor, checkpoints), not peer role.

Topology: [LEDGER_TOPOLOGY.md](../architecture/LEDGER_TOPOLOGY.md).

## Dependency

pp-cpp-amp comes in via CMake (`cmake/PpCppAmp.cmake`), pinned to a release tag there; a sibling
`../pp-cpp-amp` checkout or `PP_CPP_AMP_SOURCE_DIR` overrides the pin. Inbound protocol handlers
receive a `LinkHandle`; reach the link via `PeerLinkManager::WithLiveLink`.

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

`beacons[]` lists one or more upstream ledger endpoints (relay or terminal beacon), each an ADP multiaddr string.

Optional network anchor (see [LEDGER_TOPOLOGY.md](../architecture/LEDGER_TOPOLOGY.md#network-anchor)):

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

A multiaddr names the host by address or by name:

```text
/ip4/<ipv4>/udp/<port>/adp/1.0.0/p2p/<peer id>
/ip6/<ipv6>/udp/<port>/adp/1.0.0/p2p/<peer id>
/dns4/<host>/udp/<port>/adp/1.0.0/p2p/<peer id>     an IPv4 address of <host>
/dns6/<host>/udp/<port>/adp/1.0.0/p2p/<peer id>     an IPv6 address
/dns/<host>/udp/<port>/adp/1.0.0/p2p/<peer id>      either
```

A name is resolved when it is dialed (Amp, off the network thread), kept for a minute, and looked
up again after a failed dial, so a node that moves to another address under the same name is
followed without a config change. The peer id still authenticates the node: whoever answers at the
name must hold its identity key.

## Client (`pp-client`)

Pass the target ADP multiaddr as `-H` / `--host`:

```bash
pp-client -b -H '/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/...' status
```

## Related

- [LEDGER_TOPOLOGY.md](../architecture/LEDGER_TOPOLOGY.md) — network topology (uniform upstream, realization, sync)
- pp-cpp-amp [KEEPALIVE.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/KEEPALIVE.md) — NAT link maintenance (v0.1.2+)
- pp-cpp-amp `MemoryDatagramIo::SetReorderWindow` — in-process datagram reorder (v0.1.3+)
- Fleet integration (pp-browser / pp-node): deferred to WS3+
