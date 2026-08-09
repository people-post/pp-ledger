# Vendored libsodium

Upstream: [libsodium](https://github.com/jedisct1/libsodium) **1.0.20** (stable tarball).

Built via CMake as a static library (`sodium` / `sodium::sodium`). Do not install a system `libsodium-dev` package for this project.

CMake integration adapted from [robinlinden/libsodium-cmake](https://github.com/robinlinden/libsodium-cmake) (file list / flags); `version.h` is generated into the build tree.
