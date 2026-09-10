# Third-party dependencies

Vendored libraries that are not part of the pp-ledger source tree.

| Path | Upstream | Notes |
|------|----------|--------|
| `googletest/` | [google/googletest](https://github.com/google/googletest) v1.14.0 | Used when `-DPP_LEDGER_BUILD_TESTS=ON` |

Project-local vendors that ship with application code (JSON, CLI11, cpp-httplib) live under `src/lib/`. Crypto (libsodium + PQ natives) comes from pp-cpp-crypto.

**cpp-httplib** ([`src/lib/http/`](../src/lib/http/)): fork of 0.32. Umbrella [`httplib.h`](../src/lib/http/httplib.h) includes split headers (`config.h`, `types.h`, `server.h`, `client.h`, …). Tunables use `ServerConfig` / `ClientConfig` / `Limits` / `Timeouts` / `WebsocketConfig` (not `CPPHTTPLIB_*` tunable macros). Feature gates (`CPPHTTPLIB_OPENSSL_SUPPORT`, zlib, …) remain compile-time.
