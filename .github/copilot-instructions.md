# GitHub Copilot Instructions

This file provides context for GitHub Copilot and Copilot coding agents working on `pp-ledger`.

## Project Summary

`pp-ledger` is a C++20 blockchain with Ouroboros Proof-of-Stake consensus, written with CMake. It has no Docker/containerization. Key binaries: `pp-beacon` (validator), `pp-relay` (trusted intermediary), `pp-miner` (block producer), `pp-client` (CLI), `pp-http` (HTTP API proxy).

## Repository Layout

```
pp-ledger/
├── lib/          # Core utilities: Logger, Serialize, BinaryPack, ResultOrError
├── interface/    # Shared interfaces (IBlock, etc.)
├── consensus/    # Ouroboros PoS: Ouroboros, EpochManager, SlotLeaderSelection, SlotTimer
├── ledger/       # Blockchain storage: Ledger, FileStore, DirStore, etc.
├── network/      # TCP networking: FetchClient/Server, TcpClient/Server
├── server/       # Beacon + Relay + Miner server logic and AccountBuffer
├── client/       # TCP client library
├── http/         # HTTP API server (pp-http), built with -DBUILD_HTTP=ON
├── node-addon/   # Node.js native addon
├── app/          # Entrypoints: pp-beacon, pp-relay, pp-miner, pp-client, pp-http
├── scripts/      # Helper scripts
├── docs/         # Documentation
├── AGENTS.md     # Cursor Cloud agent instructions
└── .aicodeguide  # General AI coding conventions
```

## Build

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
make -j$(nproc)
```

- C++20, GCC 13+ or Clang 12+, CMake 3.15+
- Required: `libsodium-dev`, `build-essential`, `pkg-config`, `libstdc++-14-dev`
- Optional: `nlohmann-json3-dev` (auto-downloaded via FetchContent if absent)
- HTTP server: add `-DBUILD_HTTP=ON`

## Test

```bash
cd build && ctest --output-on-failure
```

326 tests across lib, consensus, ledger, network, and server.

## Lint

```bash
clang-tidy -p build lib/*.cpp consensus/*.cpp ledger/*.cpp network/*.cpp \
  server/*.cpp client/*.cpp app/*.cpp
```

Run lint before submitting PRs. Config is in `.clang-tidy` at repo root.

## Code Conventions

See `.aicodeguide` for the full style guide. Key points:

- **Namespace**: all code under `pp::` (sub-namespaces: `pp::consensus`, `pp::ledger`, `pp::network`, etc.)
- **Naming**: CamelCase classes, camelCase functions, trailing `_` for private members
- **Smart pointers**: `ukp` prefix for `unique_ptr`, `sp` for `shared_ptr`, `p` for raw
- **One class per file**: each class gets its own `.h` / `.cpp` pair
- **Headers**: `.h` for regular, `.hpp` for header-only/templates
- **Include guards**: `#ifndef PP_LEDGER_<COMPONENT>_H`
- **Error handling**: use `pp::ResultOrError<T>` for expected failures; exceptions for unexpected ones
- **CMake targets**: named `ppledger_<component>` (e.g., `ppledger_lib`, `ppledger_consensus`)
- When adding files: update the component's `CMakeLists.txt`

## Critical Gotchas

- **Miner config uses `"keys"` (array of file paths)**, NOT `"privateKey"`. Each file contains a 64-hex-char Ed25519 private key.
- **Beacon must be initialized with `--init` on first run**, then started without `--init` thereafter.
- **Relay does NOT require `--init`** — it auto-creates `config.json` on first run.
- **Relay config uses a `"beacon"` object** (single upstream beacon), NOT a `"beacons"` array like miners use.
- **Miners point their `"beacons"` config to relay endpoints**, not directly to beacon endpoints in production deployments.
- **HTTP API routes are prefixed with `/api/`** (e.g., `/api/beacon/state`, `/api/miner/status`). The README may not reflect this.
- **VRF leader election is probabilistic** — a miner may not be elected for many consecutive slots. This is normal.
- **`test-network.sh`** is the correct script name (not `start-test-network.sh`).

## Default Ports

| Service    | Port |
|------------|------|
| Beacon     | 8517 |
| Relay      | 8519 (configure to avoid conflict with beacon/miner) |
| Miner      | 8518 |
| HTTP API   | 8080 |

## Common Tasks (Skills)

| Task | Command |
|------|---------|
| Build | `cd build && cmake .. && make -j$(nproc)` |
| Run all tests | `cd build && ctest --output-on-failure` |
| Lint | `clang-tidy -p build lib/*.cpp ...` |
| Start test network | `./test-network.sh` |
| Init beacon (first time) | `./build/app/pp-beacon -d beacon --init` |
| Start beacon | `./build/app/pp-beacon -d beacon` |
| Start relay | `./build/app/pp-relay -d relay1` |
| Start miner | `./build/app/pp-miner -d miner1` |
| Check beacon status | `./build/app/pp-client -b status` |
| Submit transaction | `./build/app/pp-client -m add-tx <from> <to> <amount>` |
| Start HTTP API | `./build/app/pp-http --port 8080 --beacon localhost:8517 --miner localhost:8518` |

## PR Checklist

- [ ] Project builds without errors: `make -j$(nproc)`
- [ ] All tests pass: `ctest --output-on-failure`
- [ ] `clang-tidy` reports no new warnings
- [ ] `CMakeLists.txt` updated if files were added or removed
- [ ] No use of `"privateKey"` in miner config examples (use `"keys"` array instead)
