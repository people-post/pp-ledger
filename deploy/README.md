# Docker deployment

Single image (`ubuntu:24.04`, same major OS as GitHub Actions CI) containing `pp-beacon`, `pp-relay`, `pp-miner`, `pp-client`, and `pp-http`. Role is selected via the container command.

Published images: `ghcr.io/people-post/pp-ledger:<version>` (also `:latest` on tagged releases).

## Quick start (compose)

```bash
cd deploy
mkdir -p data/beacon data/relay data/miner

# Miner key (64 hex chars) + role configs (0.0.0.0 + service DNS names)
python3 -c "import os; print(os.urandom(32).hex(), end='')" > data/miner/key.txt
cp configs/relay/config.json data/relay/config.json
cp configs/miner/config.json data/miner/config.json

# Initialize beacon ledger once, then install listen config
docker compose run --rm --no-deps beacon pp-beacon -d /data --init
cp configs/beacon/config.json data/beacon/config.json

# Start the stack
docker compose up -d

curl -s http://localhost:8080/api/beacon/state
docker compose exec http pp-client --host relay:8519 -b status
```

Override the image while developing:

```bash
export PP_LEDGER_IMAGE=pp-ledger:local
docker build -t "$PP_LEDGER_IMAGE" ..
docker compose up -d
```

## Important

- Mount data directories as volumes; do not bake ledger state or private keys into the image.
- Config `"host"` must be `0.0.0.0` for cross-container access (sample configs already do this).
- Use Docker service names (`beacon`, `relay`, `miner`) in peer endpoints, not `localhost`.
- Run beacon `--init` only once against a persistent volume, then replace `config.json` with the sample that listens on all interfaces.
