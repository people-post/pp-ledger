# cpp-httplib library (header + implementation)
add_library(pp_http STATIC httplib.cc)

target_include_directories(pp_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
