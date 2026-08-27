cmake_minimum_required(VERSION 3.24)

# Top-level lib aggregator.
# Vendored json/cli; crypto (sodium + PQ) comes from pp-cpp-crypto (root).
add_subdirectory(json)
add_subdirectory(cli)
add_subdirectory(common)
