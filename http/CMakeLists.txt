# cpp-httplib library (header + implementation)
add_library(pp_http STATIC httplib_core.cc httplib_ssl.cc httplib_ws.cc)

target_include_directories(pp_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
