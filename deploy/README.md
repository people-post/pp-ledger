# Docker deployment

One image (`ubuntu:24.04`, as CI) with `pp-beacon`, `pp-relay`, `pp-miner`, `pp-client` and
`pp-http`; the container command selects the role. Published as
`ghcr.io/people-post/pp-ledger:<version>` (also `:latest` on tagged releases).

Nodes are configured from the environment ([CONFIGURATION.md § Environment](../docs/ops/CONFIGURATION.md#environment)):
no config file, and keys come from secret files (`PP_*_FILE`). The image runs as a non-root user
(uid 10001) with its work directory on the `/data` volume, logs to stderr only, and stops cleanly on
`SIGTERM`. Nodes serve no HTTP: the image's `HEALTHCHECK` asks the node over its own RPC
(`pp-client --host-file /data/listen.multiaddr health`), and pp-http serves health and Prometheus
`/metrics` for the nodes it talks to
([CONFIGURATION.md § Health and metrics](../docs/ops/CONFIGURATION.md#health-and-metrics)).

## Sample stack (compose)

Beacon → relay → miner, and the HTTP API, on one host:

```bash
cd deploy
./init.sh            # once: keys, genesis config, peer ids (.env), then genesis
docker compose up -d # waits on health: beacon, then relay, then miner, then http
curl -s http://localhost:8080/api/beacon/state
curl -s http://localhost:8080/metrics                      # beacon (the relay here), miner, core-beacon
docker compose exec relay pp-client --host-file /data/listen.multiaddr health --ready
```

`init.sh` generates every key into `secrets/` (0700), writes `secrets/init-config.json` with one
genesis miner, works out each node's peer id from its identity key (`pp-client peer-id`) into
`.env`, and runs the one-shot `beacon-init` service. Nothing is copied by hand. Variables:
`PP_LEDGER_IMAGE`, `PP_LEDGER_NETWORK_ID`, `PP_LEDGER_SLOT_DURATION`, `PP_LEDGER_SLOTS_PER_EPOCH`,
`MINER_ID`. To start over: `docker compose down -v && rm -rf secrets .env`.

The nodes reach each other by service name (`/dns4/beacon/udp/8517/adp/1.0.0/p2p/<peer id>`):
compose's DNS on the stack's network; a container that comes back at another address is looked up
again.
Host ports (UDP 8517 / 8519 / 8518, HTTP 8080) can be moved with `PP_LEDGER_*_HOST_PORT`.

Develop against a local image:

```bash
docker build -t pp-ledger:local ..
PP_LEDGER_IMAGE=pp-ledger:local ./init.sh
docker compose up -d
```

`../scripts/test/pp_ledger_local_test.sh run --suite image` runs exactly this flow (own project,
subnet and ports) and checks readiness, chain growth, the HTTP API and a clean stop.

## Production notes

- `init.sh` is a sample. Genesis, recovery and admin keys are made and kept offline by their
  holders (`pp-client keygen`); the beacon's `--init` needs only the genesis public keys in
  `init-config.json` and two genesis key files for its signature (`PP_GENESIS_KEY_FILES`).
- Node keys (`PP_BEACON_SIGNING_KEY_FILE`, `PP_LEDGER_IDENTITY_KEY_FILE`, `PP_MINER_KEYS_FILE`)
  come from a secret manager; compose mounts secret files with their host permissions, so they
  must be readable by uid 10001.
- Keep each node's `/data` on a persistent volume; never copy a beacon's `data/` to a second
  running beacon (equivocation halts the network).
- `pp-beacon --init` refuses a volume that already holds a chain (`--force` replaces it).
- The nodes get the 8 MiB UDP buffers they ask for past the host's cap through `cap_add:
  [NET_ADMIN]` (the image's node binaries hold the capability as permitted only, and use it for
  that alone). Where it cannot be granted (rootless, `no-new-privileges`), raise the host's cap
  instead (`sysctl -w net.core.rmem_max=8388608 net.core.wmem_max=8388608`, persisted in
  `/etc/sysctl.d/`; a container cannot). A node logs a warning when it got less
  ([CONFIGURATION.md § udpBufferBytes](../docs/ops/CONFIGURATION.md#udpbufferbytes-any-role)).
