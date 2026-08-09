cmake_minimum_required(VERSION 3.15)

# Top-level lib aggregator.
# Vendored deps first, then project library sources under lib/common.
add_subdirectory(json)
add_subdirectory(cli)
add_subdirectory(sodium)
add_subdirectory(common)
