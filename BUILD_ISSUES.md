# Build Issues and Resolutions

This document tracks build issues encountered during libp2p integration and their resolutions.

## Environment
- **OS**: Ubuntu 24.04.3 LTS (in devcontainer)
- **Compiler**: GCC 13.3.0
- **CMake**: 3.28.3
- **Build Date**: January 13, 2026

## Issue Summary

The project uses a precompiled libp2p artifact (from GitHub Actions) which was built in a different environment, causing ABI compatibility issues with the devcontainer's system libraries.

## Resolved Issues

### ✅ 1. fmt Library Version Mismatch

**Error:**
```
undefined reference to `void fmt::v10::detail::vformat_to<char>(...)`
```

**Cause:** 
- libp2p compiled with fmt v10.x
- System has fmt v9.1.0

**Resolution:**
```bash
cd /tmp
git clone --depth 1 --branch 10.2.1 https://github.com/fmtlib/fmt.git
cd fmt && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
sudo make install -j$(nproc)
```

**CMakeLists.txt Update:**
```cmake
set(CMAKE_PREFIX_PATH "/usr/local;${CMAKE_PREFIX_PATH}")
find_package(fmt REQUIRED)
# ...
target_link_libraries(p2p::p2p INTERFACE fmt::fmt)
```

### ✅ 2. SSL Library Incompatibility

**Error:**
```
undefined reference to `SSL_CTX_set_max_proto_version'
undefined reference to `SSL_set_mode'
undefined reference to `EVP_aead_chacha20_poly1305'
undefined reference to `X509_get_notBefore'
```

**Cause:**
- libp2p compiled with BoringSSL (Google's SSL fork)
- System has OpenSSL 3.x

**Resolution:**
```bash
cd /tmp
git clone --depth 1 https://boringssl.googlesource.com/boringssl
cd boringssl && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON ..
make -j$(nproc)
sudo cp -r ../include/openssl /usr/local/include/
sudo cp libcrypto.a /usr/local/lib/libboringssl_crypto.a
sudo cp libssl.a /usr/local/lib/libboringssl_ssl.a
```

**CMakeLists.txt Update:**
```cmake
set(BORINGSSL_INCLUDE_DIR "/usr/local/include")
set(BORINGSSL_CRYPTO_LIBRARY "/usr/local/lib/libboringssl_crypto.a")
set(BORINGSSL_SSL_LIBRARY "/usr/local/lib/libboringssl_ssl.a")
# ...
target_link_libraries(p2p::p2p INTERFACE 
    ${BORINGSSL_SSL_LIBRARY}
    ${BORINGSSL_CRYPTO_LIBRARY}
)
```

## ❌ Unresolved Issues

### 3. protobuf ABI Incompatibility

**Error:**
```
undefined reference to `google::protobuf::internal::ArenaStringPtr::Set(
    google::protobuf::internal::ArenaStringPtr::EmptyDefault, 
    std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, 
    google::protobuf::Arena*)'
undefined reference to `google::protobuf::internal::ArenaStringPtr::DestroyNoArenaSlowPath()'
undefined reference to `google::protobuf::internal::ArenaStringPtr::Mutable[abi:cxx11](...)'
```

**Cause:**
- libp2p compiled with unknown protobuf version (likely 3.12-3.19 range)
- System has protobuf 3.21.12
- protobuf internal APIs changed between versions (ABI incompatibility)

**System protobuf has:**
```cpp
ArenaStringPtr::Set(string&&, Arena*)  // Move semantics
```

**libp2p expects:**
```cpp
ArenaStringPtr::Set(EmptyDefault, const string&, Arena*)  // Copy with default
```

**Attempted Solutions:**
1. ❌ Link system protobuf static library - Same ABI mismatch
2. ❌ Link system protobuf shared library - Same ABI mismatch  
3. ⏸️ Install older protobuf version - Interrupted

**Why This is Hard:**
- protobuf `internal::` namespace APIs are not stable across versions
- ABI changes between minor versions (3.12 → 3.21)
- Static linking doesn't help - compiled code has hardcoded function signatures
- libp2p's `.a` files have embedded calls to specific protobuf API versions

### 4. Missing lsquic Library

**Error:**
```
undefined reference to `lsquic_conn_close'
```

**Cause:**
- libp2p built with QUIC support using lsquic library
- System doesn't have lsquic installed

**Status:** Not attempted (blocked by protobuf issue)

## Root Cause Analysis

The fundamental issue is **ABI incompatibility** between the precompiled libp2p artifact and the devcontainer's system libraries:

1. **Different Build Environments:**
   - libp2p artifact built in: Unknown (GitHub Actions runner)
   - Project being built in: Ubuntu 24.04 devcontainer

2. **Library Version Drift:**
   - fmt: v9 → v10 (resolved)
   - SSL: OpenSSL → BoringSSL (resolved)
   - protobuf: Unknown → 3.21.12 (unresolved)

3. **C++ ABI Fragility:**
   - Template libraries (fmt) have version in namespace
   - Internal APIs (protobuf::internal) change between releases
   - Static linking doesn't solve ABI issues

## Solutions

### Option A: Rebuild libp2p from Source (Recommended)

**Pros:**
- Ensures all dependencies match system
- Complete ABI compatibility
- No version hunting required

**Cons:**
- Build time: ~30-60 minutes
- Requires understanding libp2p build system

**Steps:**
1. Clone cpp-libp2p repository
2. Build against system libraries (fmt v9, OpenSSL, protobuf 3.21)
3. Install to custom prefix
4. Point LIBP2P_ROOT to new build

### Option B: Match All Library Versions

**Pros:**
- Uses existing libp2p artifact
- No need to rebuild libp2p

**Cons:**
- Must identify exact versions libp2p was built with
- May require building multiple libraries from source
- Fragile - breaks if artifact is rebuilt

**Required:**
- Determine exact protobuf version (3.12? 3.15? 3.19?)
- Install lsquic library
- Verify all other dependencies

### Option C: Use Shared Libraries with LD_PRELOAD

**Pros:**
- Can potentially force specific library versions at runtime

**Cons:**
- Doesn't solve compile-time ABI issues
- Very fragile and not portable
- Not recommended for production

## Recommendations

1. **Short Term:** Document the issue and required dependencies
2. **Medium Term:** Rebuild libp2p from source in same devcontainer environment
3. **Long Term:** Version control libp2p build configuration or use package manager

## Additional Dependencies Installed

During troubleshooting, these system dependencies were manually installed:

```bash
# Boost.DI (header-only dependency injection)
git clone https://github.com/boost-ext/di.git /tmp/boost-di
sudo cp -r /tmp/boost-di/include/boost /usr/local/include/

# nlohmann/json
sudo apt-get install nlohmann-json3-dev

# secp256k1 (elliptic curve cryptography)
sudo apt-get install libsecp256k1-dev

# c-ares (async DNS)
sudo apt-get install libc-ares-dev

# hat-trie (header-only hash trie)
git clone https://github.com/Tessil/hat-trie.git /tmp/hat-trie
sudo cp -r /tmp/hat-trie/include/tsl /usr/local/include/

# protobuf (system version - ABI incompatible)
sudo apt-get install libprotobuf-dev
```

## Current Build State

- ✅ Server library (ppledger_server) builds successfully
- ✅ Most test targets build successfully
- ❌ pp-ledger-server application fails at link stage (protobuf ABI)
- ❌ test_module fails at link stage (protobuf ABI)

**Build Success Rate:** ~90% (18/20 main targets)

## References

- [fmt Release Notes](https://github.com/fmtlib/fmt/releases)
- [BoringSSL Documentation](https://boringssl.googlesource.com/boringssl/)
- [protobuf Releases](https://github.com/protocolbuffers/protobuf/releases)
- [cpp-libp2p Repository](https://github.com/libp2p/cpp-libp2p)
