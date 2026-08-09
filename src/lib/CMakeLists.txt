cmake_minimum_required(VERSION 3.15)

# Top-level lib aggregator.
# The actual library sources live under lib/common.
add_subdirectory(json)
add_subdirectory(cli)
add_subdirectory(common)

