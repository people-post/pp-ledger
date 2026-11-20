# Helpers shared by the test executables (header-only).
add_library(pp_testing INTERFACE)
target_include_directories(pp_testing INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
