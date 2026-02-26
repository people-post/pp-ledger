# AGENTS.md

## Cursor Cloud specific instructions

### Overview

pp-ledger is a C++20 blockchain with Ouroboros PoS consensus. It builds with CMake and has no Docker/containerization dependencies. See `README.md` for the full quick-start guide.

### System dependencies

The following **system packages** must be present (pre-installed in the VM snapshot):

- `build-essential`, `g++` (GCC 13+)
- `cmake` (3.15+)
- `libsodium-dev`
- `libstdc++-14-dev` (required for Clang to link against libstdc++)
- `clang-tidy` (linter)
- `pkg-config`

`nlohmann-json3-dev` is optional (CMake FetchContent downloads it automatically).

### Build

Both Clang (default `c++`) and GCC (`g++`) are supported. Use `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to generate `compile_commands.json` for clang-tidy:

```bash
cd /workspace
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
make -j$(nproc)
```

### Lint

Run clang-tidy against project source files (requires `compile_commands.json` from the build):

```bash
cd /workspace
clang-tidy -p build lib/*.cpp consensus/*.cpp ledger/*.cpp network/*.cpp \
  server/*.cpp client/*.cpp app/*.cpp
```

The `.clang-tidy` config at the repo root enables bugprone, clang-analyzer, performance, and select modernize checks. Vendored code (`http/httplib.h`) is excluded via the header filter.

### Tests

```bash
cd /workspace/build && ctest --output-on-failure
```

326 tests across lib, consensus, ledger, network, and server components.

### Running the network

See `README.md` "Quick Start" section. Key gotchas:

- **Beacon must be initialized first** with `--init`. After that, run without `--init` to start.
- **Miner config** requires `"keys"` (array of key-file paths) pointing to files containing 64-hex-character Ed25519 private keys, and `"beacons"` (array of `{ "host", "port", "dhtPort" }` objects).
- The `test-network.sh` script uses `"key"` (singular string) in miner configs instead of `"keys"` (array). If this hasn't been fixed, set up miners manually — see the manual setup example below.
- Slot leader election is **VRF-based and probabilistic**; a single miner may not be elected for many consecutive slots. This is normal.

#### Manual test network

```bash
cd /workspace/build

# 1. Initialize beacon
./app/pp-beacon -d test-manual/beacon --init

# 2. Start beacon
./app/pp-beacon -d test-manual/beacon &

# 3. Create miner key and config
mkdir -p test-manual/miner1
python3 -c "import os; print(os.urandom(32).hex(), end='')" > test-manual/miner1/key.txt
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
| Miner   | 8518 |
| HTTP API| 8080 |

### HTTP API routes

Routes are prefixed with `/api/` (e.g. `/api/beacon/state`, `/api/miner/status`). The README lists routes without this prefix — add `/api/` when using curl.
