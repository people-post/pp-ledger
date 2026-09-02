# AMP ledger transport (pp-ledger)

**Status:** v1 (2026-09-01) — WS2 foundation (protocol + client/server + standalone runtime).

Cross-repo context: [platform-integration.md](platform-integration.md).

## Planes

| Plane | Role |
|-------|------|
| **AMP (UDP)** | Primary fleet transport for `/pp-ledger/rpc/1.0.0` |
| **TCP fetch** | Standalone ops (`FetchServer` / `TcpLedgerTransport`) until migrated |
| **HTTP** | Degraded client path via org relay (pp-browser; not in this module) |

## Protocol

| Field | Value |
|-------|-------|
| `protocol_id` | `/pp-ledger/rpc/1.0.0` |
| L3 policy | Control, `read_once=true`, 8 s read timeout |
| Payload | Unframed `binaryPack(Client::Request/Response)` — **no** u32 `LedgerFrameCodec` on AMP |
| Max size | 512 KiB (`ledger::rpc::kMaxPayloadBytes`) |

## Modules (pp-ledger)

| File | Role |
|------|------|
| `network/LedgerRpcProtocol.h` | Protocol id, channel policy factory, limits |
| `network/AmpLedgerServer.*` | Bind handler on `PeerLinkManager` |
| `client/AmpLedgerTransport.*` | `ILedgerTransport` over AMP |
| `network/LedgerAmpRuntime.*` | Slim `AmpStack` for standalone binaries |
| `network/ServerAmpSupport.*` | `Server::startAmpServer` glue |

Build with `-DPP_LEDGER_AMP=ON` (auto-enabled when sibling `../pp-cpp-amp` exists).

## Server usage

```cpp
#ifdef PP_LEDGER_HAS_AMP
network::LedgerAmpConfig amp_cfg;
amp_cfg.identity = ...; // ML-DSA MshIdentity
amp_cfg.local_peer_id = "...";
amp_cfg.udp_port = 8519;
startAmpServer(amp_cfg);  // dispatches via dispatchUnframedRequest
#endif
```

TCP and AMP may coexist (`startFetchServer` + `startAmpServer`) during migration.

## Config sketch (future)

```json
{
  "ledger": {
    "transport": "amp",
    "listen": "/ip4/0.0.0.0/udp/8519/adp/1.0.0/p2p/<PeerId>",
    "bootstrap_multiaddrs": ["..."]
  }
}
```

Legacy TCP `host` / `port` remains valid when `"transport": "tcp"`.

## Related

- pp-cpp-amp [KEEPALIVE.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/KEEPALIVE.md) — NAT link maintenance
- Fleet integration (pp-browser / pp-node): deferred to WS3+
