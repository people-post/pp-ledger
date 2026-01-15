# Interface library (header-only)
add_library(pp_interface INTERFACE)

target_include_directories(pp_interface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
