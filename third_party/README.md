# Third-party dependencies

Vendored libraries that are not part of the pp-ledger source tree.

| Path | Upstream | Notes |
|------|----------|--------|
| `googletest/` | [google/googletest](https://github.com/google/googletest) v1.14.0 | Used when `-DBUILD_TESTING=ON` |

Project-local vendors that ship with application code (JSON, libsodium, CLI11, cpp-httplib) live under `src/lib/` instead.
