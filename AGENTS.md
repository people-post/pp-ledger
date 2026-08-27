# AGENTS.md

## Cursor Cloud specific instructions

### Overview

pp-ledger is a C++20 blockchain with Ouroboros PoS consensus. Key binaries: `pp-beacon` (validator), `pp-relay` (trusted intermediary), `pp-miner` (block producer), `pp-client` (CLI), `pp-http` (HTTP API proxy). It builds with CMake. Optional Docker packaging uses `ubuntu:24.04` (same OS as CI) — see `Dockerfile` and `deploy/README.md`. See `README.md` for the full quick-start guide.

**Cross-repo work** (pp-cpp-common, pp-browser, pp-node integration, libp2p transport, role matrix): see [`docs/platform-integration.md`](docs/platform-integration.md).

### System dependencies

The following **system packages** must be present (pre-installed in the VM snapshot):

- `build-essential`, `g++` (GCC 13+)
- `cmake` (3.15+)
- `libstdc++-14-dev` (required for Clang to link against libstdc++)
- `clang-tidy` (linter)

JSON is vendored under `src/lib/json`. Shared foundation comes from **pp-cpp-common** and crypto from **pp-cpp-crypto** (sibling checkouts or FetchContent tags). Do not install `nlohmann-json3-dev` or `libsodium-dev` for this project. GoogleTest is vendored under `third_party/googletest` (no FetchContent / network fetch at configure time for gtest).

### Build

Both Clang (default `c++`) and GCC (`g++`) are supported. Use `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to generate `compile_commands.json` for clang-tidy:

```bash
cd /workspace
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
make -j$(nproc)
```

- `BUILD_HTTP` (default OFF): Build the HTTP API server (pp-http). Use `-DBUILD_HTTP=ON` to enable it.

### Lint

Run clang-tidy against project source files (requires `compile_commands.json` from the build):

```bash
cd /workspace
clang-tidy -p build src/lib/**/*.cpp src/consensus/*.cpp src/ledger/*.cpp \
 src/network/*.cpp src/chain/*.cpp src/server/*.cpp src/client/*.cpp src/app/*.cpp
```

The `.clang-tidy` config at the repo root enables bugprone, clang-analyzer, performance, and select modernize checks. Vendored code under `src/lib/{http,json,cli,sodium}` is excluded via the header filter.

### Tests

```bash
cd /workspace/build && ctest --output-on-failure
```

326 tests across lib, consensus, ledger, network, and server components.

### Running the network

See `README.md` "Quick Start" section. Key gotchas:

- **Beacon must be initialized first** with `--init`. After that, run without `--init` to start.
- **Miner config** requires `"keys"` (array of key-file paths) pointing to files containing hex-encoded ML-DSA-65 private keys (8064 hex chars), and `"beacons"` (array of `{ "host", "port", "dhtPort" }` objects).
- The `test-network.sh` script uses `"key"` (singular string) in miner configs instead of `"keys"` (array). If this hasn't been fixed, set up miners manually — see the manual setup example below.
- Slot leader election is **VRF-based and probabilistic**; a single miner may not be elected for many consecutive slots. This is normal.

#### Relay server

The **relay server** (`pp-relay`) sits between the beacon and miners. Miners connect to the relay instead of the beacon directly. Start with:

```bash
./app/pp-relay -d relay1
```

Config (`relay1/config.json`) is auto-created on first run. Edit it to set the upstream `beacon` endpoint:

```json
{
  "host": "localhost",
  "port": 8519,
  "dhtPort": 0,
  "beacon": { "host": "localhost", "port": 8517, "dhtPort": 0 }
}
```

Point miners' `beacons` config entries at the relay endpoint (e.g. `localhost:8519`) rather than directly at the beacon.

#### Manual test network

```bash
cd /workspace/build

# 1. Initialize beacon
./app/pp-beacon -d test-manual/beacon --init

# 2. Start beacon
./app/pp-beacon -d test-manual/beacon &

# 3. Create miner key and config
mkdir -p test-manual/miner1
./app/pp-client keygen | tee /tmp/keygen.out
grep 'Private key' /tmp/keygen.out | sed 's/.*: *//' | tr -d ' \n' > test-manual/miner1/key.txt
cat > test-manual/miner1/config.json << 'EOF'
{
  "minerId": 1,
  "keys": ["/workspace/build/test-manual/miner1/key.txt"],
  "host": "localhost",
  "port": 8518,
  "beacons": [{"host":"localhost","port":8517,"dhtPort":0}]
}
EOF

# 4. Start miner
./app/pp-miner -d test-manual/miner1 &

# 5. Start HTTP API (optional)
./app/pp-http --port 8080 --beacon localhost:8517 --miner localhost:8518 &
```

### Default ports

| Service | Port |
|---------|------|
| Beacon  | 8517 |
| Relay   | 8519 (configure to avoid conflict with beacon/miner) |
| Miner   | 8518 |
| HTTP API| 8080 |

### HTTP API routes

Routes are prefixed with `/api/` (e.g. `/api/beacon/state`, `/api/miner/status`). The README lists routes without this prefix — add `/api/` when using curl.
