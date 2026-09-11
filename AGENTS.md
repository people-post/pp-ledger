# AGENTS.md

## Cursor Cloud specific instructions

### Overview

pp-ledger is a C++20 blockchain with **SlotCommittee** leader election (equal-weight
top‑N stake committee under beacon authority; Ouroboros is a design reference only).
Key binaries: `pp-beacon` (validator), `pp-relay` (trusted intermediary), `pp-miner`
(block producer), `pp-client` (CLI), `pp-http` (HTTP API proxy). It builds with CMake.
Optional Docker packaging uses `ubuntu:24.04` (same OS as CI) — see `Dockerfile` and
`deploy/README.md`. See `README.md` for the full quick-start guide.

**Cross-repo work** (pp-cpp-common, pp-browser, pp-node integration, libp2p transport, role matrix): see [`docs/architecture/PLATFORM_INTEGRATION.md`](docs/architecture/PLATFORM_INTEGRATION.md).

**Name directory / domains** (memorable `local@domain`, reserved-account domain ownership — design only): see [`docs/product/NAME_DIRECTORY.md`](docs/product/NAME_DIRECTORY.md).

### System dependencies

The following **system packages** must be present (pre-installed in the VM snapshot):

- `build-essential`, `g++` (GCC 13+)
- `cmake` (3.15+)
- `libstdc++-14-dev` (required for Clang to link against libstdc++)
- `clang-tidy` (linter)

Value/Meta JSON IO lives in **pp-cpp-common** (`common/Value.h`, `common/io/Json.h`). Shared foundation and crypto come from **pp-cpp-common** and **pp-cpp-crypto** via CMake FetchContent (tags `v0.2.0` / `v0.1.0`). **pp-cpp-amp** (`v0.1.6`) is required for fleet networking — see [docs/contracts/AMP_TRANSPORT.md](docs/contracts/AMP_TRANSPORT.md). Amp is transport-only; ledger RPC/sync stay in this repo. Optional override: `-DPP_CPP_COMMON_SOURCE_DIR=` / `-DPP_CPP_CRYPTO_SOURCE_DIR=` / `-DPP_CPP_AMP_SOURCE_DIR=`. Do not install `nlohmann-json3-dev` or `libsodium-dev` for this project. GoogleTest is vendored under `third_party/googletest`.

### Build

Both Clang (default `c++`) and GCC (`g++`) are supported. Use `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to generate `compile_commands.json` for clang-tidy:

```bash
cd /workspace
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
make -j$(nproc)
```

- `PP_LEDGER_BUILD_HTTP` (default OFF): Build the HTTP API server (pp-http).
- `PP_LEDGER_BUILD_APPS` (default ON): Build `pp-beacon`, `pp-relay`, `pp-miner`, `pp-client`.
- `PP_LEDGER_BUILD_TESTS` (default OFF): Build and enable ctest.

### Lint

Run clang-tidy against project source files (requires `compile_commands.json` from the build):

```bash
cd /workspace
clang-tidy -p build src/lib/**/*.cpp src/consensus/*.cpp src/ledger/*.cpp \
 src/network/*.cpp src/chain/*.cpp src/server/*.cpp src/client/*.cpp src/app/*.cpp
```

The `.clang-tidy` config at the repo root enables bugprone, clang-analyzer, performance, and select modernize checks. Vendored code under `src/lib/{http,json,cli,sodium}` is excluded via the header filter.

### Tests

Doctrine + purpose catalog: [docs/architecture/TESTING.md](docs/architecture/TESTING.md), [docs/ops/TEST_STRATEGY.md](docs/ops/TEST_STRATEGY.md).

```bash
cd /workspace/build && ctest --output-on-failure
# or:
./scripts/ci-build.sh --test
```

Multi-process smoke (L0 / L1 / LATEJOIN):

```bash
./scripts/test/pp_ledger_local_test.sh run --suite unit
./scripts/test/pp_ledger_local_test.sh run --suite l0
./scripts/test/pp_ledger_local_test.sh run --suite smoke --down   # == l0 until Amp dial green
```

See [docs/ops/TEST_STRATEGY.md](docs/ops/TEST_STRATEGY.md) for purpose IDs and script map.
L0 multi-process smoke is green on localhost OsUdp; L1/LATEJOIN remain slot-lottery flaky
until out-of-process forced-leader exists. L0 fail-fast layers: PIDs → beacon RPC → miner RPC;
artifacts under `build/test-smoke/artifacts/` on failure.

### Running the network

Prefer the smoke harness for local fleets:

```bash
./scripts/test/pp_ledger_local_test.sh up
./scripts/test/pp_ledger_local_test.sh status
./scripts/test/pp_ledger_local_test.sh clear
```

Manual bring-up gotchas (see also [docs/contracts/AMP_TRANSPORT.md](docs/contracts/AMP_TRANSPORT.md)):

- **Beacon must be initialized first** with `--init`. After that, run without `--init` to start.
- Peer dialing uses **ADP multiaddrs** (`/ip4/.../udp/.../adp/1.0.0/p2p/<peer-id>`), not host:port alone. Each server logs `AMP ledger listener: ...` on start — copy that into relay `beacon`, miner `beacons[]`, and `pp-client --host`.
- **Miner config** requires `"keys"` (array of key-file paths) and `"beacons"` (array of multiaddr strings or `{host,port,peerId}` objects).
- Slot leader election is probabilistic; empty slots are normal. Smoke L1 uses short slots + tx inject and may be flaky until forced-leader is available out-of-process.

#### Relay server

The **relay server** (`pp-relay`) sits between the beacon and miners. Start with:

```bash
./app/pp-relay -d relay1
```

Config example (multiaddr form):

```json
{
  "port": 8519,
  "keys": ["keys/amp-identity.txt"],
  "beacon": "/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon-peer-id>"
}
```

#### Manual test network

```bash
cd /workspace/build

# 1. Initialize beacon
./app/pp-beacon -d test-manual/beacon --init

# 2. Start beacon; copy AMP ledger listener multiaddr from logs
./app/pp-beacon -d test-manual/beacon &

# 3. Create miner key and config (beacons[] = relay or beacon multiaddr)
mkdir -p test-manual/miner1
./app/pp-client keygen | tee /tmp/keygen.out
grep 'Private key' /tmp/keygen.out | sed 's/.*: *//' | tr -d ' \n' > test-manual/miner1/key.txt
cat > test-manual/miner1/config.json << 'EOF'
{
  "minerId": 1,
  "keys": ["key.txt"],
  "host": "127.0.0.1",
  "port": 8518,
  "beacons": ["/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon-peer-id>"]
}
EOF

# 4. Start miner
./app/pp-miner -d test-manual/miner1 &

# 5. HTTP API (optional; needs ADP multiaddrs)
./app/pp-http --port 8080 \
  --beacon '/ip4/127.0.0.1/udp/8517/adp/1.0.0/p2p/<beacon-peer-id>' \
  --miner '/ip4/127.0.0.1/udp/8518/adp/1.0.0/p2p/<miner-peer-id>' &
```

### Default ports

| Service | Port (dogfood) | Smoke harness |
|---------|----------------|---------------|
| Beacon  | 8517 | 8617 |
| Relay   | 8519 | 8622 |
| Miner   | 8518 | 8618+ |
| HTTP API| 8080 | 8680 |

Image smoke scaffold: `deploy/compose.smoke.yml` (ports 8617+; Amp multiaddr placeholders).

### HTTP API routes

Routes are prefixed with `/api/` (e.g. `/api/beacon/state`, `/api/miner/status`). The README lists routes without this prefix — add `/api/` when using curl.
