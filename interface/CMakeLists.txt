# Interface library (header-only)
add_library(interface INTERFACE)

target_include_directories(interface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
