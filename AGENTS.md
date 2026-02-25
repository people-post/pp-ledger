# AGENTS.md

## Cursor Cloud specific instructions

### Overview

pp-ledger is a C++20 blockchain with Ouroboros PoS consensus. It builds with CMake and has no Docker/containerization dependencies. See `README.md` for the full quick-start guide.

### System dependencies

The following **system packages** must be present (pre-installed in the VM snapshot):

- `build-essential`, `g++` (GCC 13+)
- `cmake` (3.15+)
- `libsodium-dev`
- `pkg-config`

`nlohmann-json3-dev` is optional (CMake FetchContent downloads it automatically).

### Build

The default `c++` symlink points to **Clang**, which fails to link against libstdc++. Always specify GCC explicitly:

```bash
cd /workspace
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ ..
make -j$(nproc)
```

### Tests

```bash
cd /workspace/build && ctest --output-on-failure
```

326 tests across lib, consensus, ledger, network, and server components.

### Running the network

See `README.md` "Quick Start" section. Key gotchas:

- **Beacon must be initialized first** with `--init`. After that, run without `--init` to start.
- **Miner config** requires `"keys"` (array of key-file paths) pointing to files containing 64-hex-character Ed25519 private keys, and `"beacons"` (array of `"host:port"` strings).
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
  "beacons": ["localhost:8517"]
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
