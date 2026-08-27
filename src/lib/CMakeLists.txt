cmake_minimum_required(VERSION 3.24)

# Top-level lib aggregator.
# Vendored cli; crypto (sodium + PQ) comes from pp-cpp-crypto (root).
# Value/Meta/JSON live in pp-cpp-common.
add_subdirectory(cli)
add_subdirectory(common)
