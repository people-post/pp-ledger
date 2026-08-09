cmake_minimum_required(VERSION 3.15)

# Vendored CLI11 2.6.2: multi-header under include/CLI + Precompile.cpp (always precompiled).
add_library(CLI11 STATIC src/Precompile.cpp)
add_library(CLI11::CLI11 ALIAS CLI11)

target_compile_definitions(CLI11 PUBLIC CLI11_COMPILE)

target_include_directories(CLI11 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
