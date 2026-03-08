# Setup Guide

This guide covers detailed setup and configuration for all pp-ledger components.

## Beacon Server

The beacon server is the network validator and authoritative data source.

### Mode 1: Initialize a new beacon (first-time setup)

```bash
cd build
mkdir -p beacon
./app/pp-beacon -d beacon --init
```

This will:
- Create `beacon/init-config.json` with default consensus parameters if it doesn't exist
- Initialize the beacon with genesis block (block 0)
- Create `beacon/config.json` for runtime configuration

You can customize `beacon/init-config.json` before initialization:
```json
{
  "slotDuration": 5,           // Slot duration in seconds (default: 5)
  "slotsPerEpoch": 432,        // Slots per epoch (default: 432 = ~36 minutes)
  "checkpointSize": 1073741824,  // Checkpoint size in bytes (default: 1GB)
  "checkpointAge": 31536000    // Checkpoint age in seconds (default: 1 year)
}
```

### Mode 2: Mount an existing beacon

```bash
cd build
./app/pp-beacon -d beacon
```

The beacon will:
- Create `beacon/ledger/` directory for blockchain data
- Create `beacon/beacon.log` for detailed logs
- Listen on the configured host and port (default: `localhost:8517`)
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

On first run, the miner creates a default `miner1/config.json`. Edit it to configure your miner:

```json
{
  "minerId": 1,
  "keys": ["miner1/key.txt"],
  "host": "localhost",
  "port": 8518,
  "beacons": [{"host":"127.0.0.1","port":8517,"dhtPort":0}]
}
```

Config fields:
- `minerId` (required): Unique numeric miner identifier
- `keys` (required): Array of key-file paths containing 64-hex-character Ed25519 private keys
- `host` (optional): Listen address, default: `"localhost"`
- `port` (optional): Listen port, default: `8518`
- `beacons` (required): List of beacon endpoints `{host, port, dhtPort}` to connect to

The miner will:
- Connect to the beacon(s) specified in config
- Create `miner1/ledger/` directory for blockchain data
- Create `miner1/miner.log` for detailed logs
- Listen on the configured host and port (default: `localhost:8518`)
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
./app/pp-client -m add-tx alice bob 100

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
./app/pp-client -h 192.168.1.100 -p 8518 -m add-tx wallet1 wallet2 500
```

---

## HTTP API Server (pp-http)

The HTTP server exposes the same interfaces as the client over REST-style HTTP, proxying to configured beacon and miner endpoints.

### Build and run

```bash
cd build
cmake -DBUILD_HTTP=ON ..   # Re-run cmake to enable the HTTP server (off by default)
make pp-http
./app/pp-http --port 8080 --beacon localhost:8517 --miner localhost:8518
```

### Options

- `--port <port>` — HTTP listen port (default: 8080)
- `--bind <address>` — Bind address (default: 0.0.0.0)
- `--beacon <host:port>` — Beacon endpoint (default: localhost:8517)
- `--miner <host:port>` — Miner endpoint (default: localhost:8518)

### Routes

All routes are prefixed with `/api/`.

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/beacon/state` | Beacon state (checkpoint, block, slot, epoch, timestamp) |
| GET | `/api/beacon/timestamp` | Server timestamp in ms (for calibration) |
| GET | `/api/beacon/miners` | Miner list from beacon |
| GET | `/api/miner/status` | Miner status (stake, nextBlockId, pending txs, etc.) |
| GET | `/api/block/<id>` | Block by ID (JSON) |
| GET | `/api/account/<id>` | User account by ID (JSON) |
| GET | `/api/tx/by-wallet?walletId=&beforeBlockId=` | Transactions by wallet |
| POST | `/api/tx` | Submit transaction (body: binary packed `SignedData<Transaction>`; use the `node-addon` or client library to construct this payload) |

### Examples

```bash
curl http://localhost:8080/api/beacon/state
curl http://localhost:8080/api/miner/status
curl "http://localhost:8080/api/tx/by-wallet?walletId=1048576&beforeBlockId=10"
```

---

## Multi-Node Setup

To run multiple nodes for a distributed network:

**Beacon 1:**
```bash
mkdir -p beacon1
./app/pp-beacon -d beacon1 --init
cat > beacon1/config.json << EOF
{
  "host": "localhost",
  "port": 8517,
  "beacons": ["localhost:8527"]
}
EOF
./app/pp-beacon -d beacon1
```

**Beacon 2:**
```bash
mkdir -p beacon2
cp -r beacon1/ledger beacon2/
cat > beacon2/config.json << EOF
{
  "host": "localhost",
  "port": 8527,
  "beacons": ["localhost:8517"]
}
EOF
./app/pp-beacon -d beacon2
```

**Miner 1:**
```bash
mkdir -p miner1
cat > miner1/config.json << EOF
{
  "minerId": 1,
  "keys": ["miner1/key.txt"],
  "host": "localhost",
  "port": 8518,
  "beacons": [{"host":"localhost","port":8517,"dhtPort":0},{"host":"localhost","port":8527,"dhtPort":0}]
}
EOF
./app/pp-miner -d miner1
```

**Miner 2:**
```bash
mkdir -p miner2
cat > miner2/config.json << EOF
{
  "minerId": 2,
  "keys": ["miner2/key.txt"],
  "host": "localhost",
  "port": 8528,
  "beacons": [{"host":"localhost","port":8517,"dhtPort":0},{"host":"localhost","port":8527,"dhtPort":0}]
}
EOF
./app/pp-miner -d miner2
```

---

## Configuration Reference

### Beacon `init-config.json` (for `--init` mode)

```json
{
  "slotDuration": 5,             // Slot duration in seconds (default: 5)
  "slotsPerEpoch": 432,          // Slots per epoch (default: 432 = ~36 min)
  "checkpointSize": 1073741824,  // Checkpoint size in bytes (default: 1GB)
  "checkpointAge": 31536000      // Checkpoint age in seconds (default: 1 year)
}
```

### Beacon `config.json` (runtime)

```json
{
  "host": "localhost",           // Optional, default: "localhost"
  "port": 8517,                  // Optional, default: 8517
  "beacons": [                   // Optional, list of other beacon addresses
    "host1:port1",
    "host2:port2"
  ],
  "checkpointSize": 1073741824,  // Optional, default: 1GB
  "checkpointAge": 31536000      // Optional, default: 1 year
}
```

### Miner `config.json`

```json
{
  "minerId": 1,                   // Required, unique numeric identifier
  "keys": ["key.txt"],            // Required, key files for signing (multiple = multiple signatures)
  "host": "localhost",            // Optional, default: "localhost"
  "port": 8518,                   // Optional, default: 8518
  "beacons": [                    // Required, list of beacon addresses to connect to
    {"host":"localhost","port":8517,"dhtPort":0}
  ]
}
```

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
