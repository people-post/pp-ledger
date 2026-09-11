# Docker deployment

Single image (`ubuntu:24.04`, same major OS as GitHub Actions CI) containing `pp-beacon`, `pp-relay`, `pp-miner`, `pp-client`, and `pp-http`. Role is selected via the container command.

Published images: `ghcr.io/people-post/pp-ledger:<version>` (also `:latest` on tagged releases).

## Quick start (compose)

```bash
cd deploy
mkdir -p data/beacon/keys data/relay/keys data/miner

# Miner key (hex private key from pp-client keygen) + Amp-shaped role configs
# Replace REPLACE_*_PEER_ID in configs/{relay,miner}/config.json after listeners publish.
cp configs/relay/config.json data/relay/config.json
cp configs/miner/config.json data/miner/config.json
# Write relay Amp identity (hex) to data/relay/keys/amp-identity.txt before start.

# Initialize beacon ledger once, then install listen config
docker compose run --rm --no-deps beacon pp-beacon -d /data --init
cp configs/beacon/config.json data/beacon/config.json

# Start the stack; copy each `AMP ledger listener:` multiaddr into peer configs / http command
docker compose up -d

curl -s http://localhost:8080/api/beacon/state
docker compose exec http pp-client --host '/ip4/relay/udp/8519/adp/1.0.0/p2p/<relay-peer-id>' -b status
```

Override the image while developing:

```bash
export PP_LEDGER_IMAGE=pp-ledger:local
docker build -t "$PP_LEDGER_IMAGE" ..
docker compose up -d
```

## Smoke compose scaffold

Ports **8617+** and Amp multiaddr placeholders: [`compose.smoke.yml`](compose.smoke.yml) + [`configs/smoke/`](configs/smoke/). Local process harness is preferred (`../scripts/test/pp_ledger_local_test.sh`); `--suite image` validates the scaffold when `PP_LEDGER_SMOKE_IMAGE_RUN=1`.

## Important

- Mount data directories as volumes; do not bake ledger state or private keys into the image.
- Config `"host"` must be `0.0.0.0` for cross-container access (sample configs already do this).
- Peer dialing uses **ADP multiaddrs** (`/ip4/<service>/udp/<port>/adp/1.0.0/p2p/<peer-id>`), not bare `host:port`.
- Use Docker service names (`beacon`, `relay`, `miner`) in multiaddr host segments, not `localhost`.
- Run beacon `--init` only once against a persistent volume, then replace `config.json` with the sample that listens on all interfaces.
