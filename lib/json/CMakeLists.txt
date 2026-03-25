cmake_minimum_required(VERSION 3.15)

# Vendored nlohmann/json single-header.
# Header path: lib/json/json.hpp
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

target_include_directories(nlohmann_json INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

