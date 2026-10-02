# Setup Guide

This guide covers detailed setup and configuration for all pp-ledger components.

## Beacon Server

The beacon server is the network validator and authoritative data source.

### Mode 1: Initialize a new beacon (first-time setup)

Every account holder makes its own keys; the beacon only receives public keys.

```bash
cd build
# Each holder, on their own machine (genesis: 3 holders, any 2 sign):
./app/pp-client keygen -o genesis1     # genesis2, genesis3, fee, reserve, recycle likewise
mkdir -p beacon
./app/pp-beacon -d beacon --init       # writes a beacon/init-config.json template and stops
# fill in networkId and the public keys (*.pub), then:
./app/pp-beacon -d beacon --init --genesis-key genesis1.key --genesis-key genesis2.key
```

This creates the genesis block (block 0) declaring the system accounts and any
genesis miners by public key. The genesis keys are the chain's admin keys:
keep them offline. Fields and rules: [CONFIGURATION.md](CONFIGURATION.md).

### Mode 2: Mount an existing beacon

```bash
cd build
./app/pp-beacon -d beacon
```

The beacon will:
- Create `beacon/ledger/` directory for blockchain data
- Create `beacon/beacon.log` for detailed logs
- Create its network identity `keys/amp-identity.txt` on first start and print its listen multiaddr
- Listen on the configured UDP port (default: 8517)
- Validate blocks (but does NOT produce blocks)

### Debug mode

```bash
./app/pp-beacon -d beacon --debug
```

---

## Miner Server

The miner server produces blocks when elected as slot leader and maintains a transaction pool.

### Setup

```bash
cd build
mkdir -p miner1
./app/pp-miner -d miner1
```

On first run, the miner creates a default `miner1/config.json`. Edit it:

```json
{
  "minerId": 1048576,
  "keys": ["key.txt"],
  "beacons": ["/ip4/10.0.0.2/udp/8519/adp/1.0.0/p2p/<relay peer id>"]
}
```

Fields and defaults: [CONFIGURATION.md](CONFIGURATION.md).

The miner will:
- Connect to the beacon(s) specified in config
- Create `miner1/ledger/` directory for blockchain data
- Create `miner1/miner.log` for detailed logs
- Dial out to its upstream(s); nothing needs to reach the miner
- Automatically produce blocks when elected as slot leader (if there are pending transactions)

### Debug mode

```bash
./app/pp-miner -d miner1 --debug
```

---

## Client

The client connects to either the beacon server or miner server to query status and send commands.

### Beacon commands

```bash
# Get beacon status (shows current block, slot, epoch, stakeholders)
./app/pp-client -b status

# Get block by ID
./app/pp-client -b block 0

# Get slot leader for a specific slot
./app/pp-client -b slot-leader 100
```

### Miner commands

```bash
# Get miner status (shows miner ID, stake, current slot, pending txs)
./app/pp-client -m status

# Add a transaction to the pending pool
./app/pp-client -b add-tx alice bob 100   # to a relay or the beacon

# Manually trigger block production (for testing)
./app/pp-client -m produce-block
```

### Options

- `-h <host>` — Server host (default: localhost)
- `-h <host:port>` — Server host and port in one argument
- `-p <port>` — Server port (overrides default)
- `-b` — Connect to BeaconServer (default port: 8517)
- `-m` — Connect to MinerServer (default port: 8518)
- `--debug` — Enable debug logging

### Examples

```bash
# Connect to beacon on default port
./app/pp-client -b status

# Connect to beacon on custom port
./app/pp-client -b -p 8527 status

# Connect to beacon on custom host and port
./app/pp-client -h beacon.example.com:8517 -b status

# Connect to miner on default port
./app/pp-client -m status

# Connect to miner on custom host
./app/pp-client -h 192.168.1.100 -p 8622 -b add-tx wallet1 wallet2 500   # relay
```

---

## HTTP API Server (pp-http)

The HTTP server exposes the same interfaces as the client over REST-style HTTP, proxying to configured beacon and miner endpoints over **AMP** (same transport as `pp-client`).

### Build and run

```bash
cd build
cmake -DPP_LEDGER_BUILD_HTTP=ON ..   # Re-run cmake to enable the HTTP server (off by default)
make pp-http
# Copy listen multiaddrs from beacon/miner logs (adp/1.0.0/p2p/...)
./app/pp-http --port 8080 \
  --beacon '/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon-peer-id>' \
  --miner  '/ip4/127.0.0.1/udp/8518/adp/1.0.0/p2p/<miner-peer-id>'
```

### Options

- `--port <port>` — HTTP listen port (default: 8080)
- `--bind <address>` — Bind address (default: 0.0.0.0)
- `--beacon <multiaddr>` — Beacon ADP multiaddr (required)
- `--miner <multiaddr>` — Miner ADP multiaddr (required)

### Routes

All routes are prefixed with `/api/`.

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/beacon/state` | Beacon state (checkpoint, block, slot, epoch, timestamp) |
| GET | `/api/beacon/calibration` | Beacon calibration data |
| GET | `/api/beacon/miners` | Registered miner ids and last renewal (no network addresses) |
| GET | `/api/miner/status` | Miner status (stake, nextBlockId, pending txs, etc.) |
| GET | `/api/block/<id>` | Block by ID (JSON) |
| GET | `/api/account/<id>` | User account by ID (JSON) |
| POST | `/api/account/create` | Create account (JSON body; may sign with provided key) |
| GET | `/api/tx/by-wallet?walletId=&beforeBlockId=` | Transactions by wallet |
| GET | `/api/tx/by-index?txIndex=` | Transaction by global index |
| POST | `/api/tx/build` | Build unsigned transaction hex from JSON |
| POST | `/api/tx/submit` | Submit signed transaction (`type`, `transactionHex`, `signaturesHex[]`) |

External integrations should use this HTTP API (or `pp-client`) rather than native language bindings.

### Examples

```bash
curl http://localhost:8080/api/beacon/state
curl http://localhost:8080/api/miner/status
curl "http://localhost:8080/api/tx/by-wallet?walletId=1048576&beforeBlockId=10"
curl -X POST http://localhost:8080/api/tx/build -H 'Content-Type: application/json' -d '{"type":2,...}'
```

---

## Multi-Node Setup

One beacon (the terminal), relays below it, miners below the relays. Miners
only dial out; give them relay multiaddrs. `scripts/test/pp_ledger_local_test.sh`
does all of this locally (`run --suite smoke`).

**1. Miner keys** (each miner operator, before genesis):
```bash
./app/pp-client keygen        # keep the private key; send the public key to the beacon operator
```

**2. Beacon** — system accounts (Mode 1 above) and the miners in
`init-config.json`, then init and start:
```bash
mkdir -p beacon
cat > beacon/init-config.json << 'JSON'
{
  "networkId": "my-network",
  "systemAccounts": {"genesis": {"publicKeys": ["<g1>", "<g2>", "<g3>"], "minSignatures": 2},
                     "fee": {"publicKeys": ["<fee>"]}, "reserve": {"publicKeys": ["<reserve>"]},
                     "recycle": {"publicKeys": ["<recycle>"]}},
  "genesisMiners": [
    {"id": 1048576, "publicKeys": ["<miner 1 public key hex>"]},
    {"id": 1048577, "publicKeys": ["<miner 2 public key hex>"]}
  ]
}
JSON
./app/pp-beacon -d beacon --init --genesis-key genesis1.key --genesis-key genesis2.key
./app/pp-beacon -d beacon     # prints "AMP ledger listener: /ip4/.../p2p/<beacon peer id>"
```

**3. Relay** — upstream is the beacon's multiaddr:
```bash
mkdir -p relay
echo '{"beacon": "/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon peer id>"}' > relay/config.json
./app/pp-relay -d relay       # prints its own listen multiaddr
```

**4. Miners** — each with its account id, private key and the relay's multiaddr:
```bash
mkdir -p miner1 && cp <miner-1-private-key-file> miner1/key.txt
cat > miner1/config.json << 'JSON'
{
  "minerId": 1048576,
  "keys": ["key.txt"],
  "beacons": ["/ip4/127.0.0.1/udp/8519/adp/1.0.0/p2p/<relay peer id>"]
}
JSON
./app/pp-miner -d miner1
```

---

## Configuration Reference

See [CONFIGURATION.md](CONFIGURATION.md) — every field of `init-config.json` and
each role's `config.json`, and the fixed network timing.

---

## Troubleshooting

**"Failed to start beacon"**
- Ensure the work directory exists
- For first-time setup, use `--init` flag to initialize the beacon
- For existing beacon, ensure `config.json` exists in the work directory
- Check that the port is not already in use: `netstat -tuln | grep <port>`

**"Failed to initialize beacon"**
- Ensure you have write permissions in the work directory
- Check that `init-config.json` has valid JSON format
- Review the error message in console or `beacon.log`

**"Failed to start miner"**
- Ensure at least one beacon is running and accessible
- Verify beacon addresses in `config.json` are correct and reachable
- Check that the miner's port is not already in use

**"Failed to connect to beacon"**
- Ensure the beacon server is running: `./app/pp-client -b status`
- Check the beacon address and port in miner's `config.json`
- Verify network connectivity: `telnet <beacon_host> <beacon_port>`
- Check firewall settings if running on different machines

**"Failed to open index file for writing"**
- Ensure you have write permissions in the work directory
- The ledger subdirectory will be created automatically
- Check disk space availability

**Port already in use**
- Change the port number in `config.json`
- Or stop the process using that port: `lsof -ti:<port> | xargs kill`

**Blocks not being produced**
- Ensure the miner has pending transactions: `./app/pp-client -m status`
- Check that the miner is registered as a stakeholder with the beacon
- Slot leader selection is probabilistic based on stake (may need to wait several slots)
- Review logs in `<work-dir>/miner.log` with `--debug` flag for details
