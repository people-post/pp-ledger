cmake_minimum_required(VERSION 3.15)

# Vendored nlohmann/json 3.12.0: modular headers under include/nlohmann + json.hpp shim.
# Header-only (no separate compilation target; unlike CLI11 there is no upstream .cpp mode).
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

target_include_directories(nlohmann_json INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
