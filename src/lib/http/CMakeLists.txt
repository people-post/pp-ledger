# cpp-httplib library (header + implementation)
add_library(pp_http STATIC httplib_core.cc httplib_ssl.cc httplib_ws.cc)

# SYSTEM so httplib.h does not trigger -Werror in dependents.
target_include_directories(pp_http SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Upstream warnings must not fail our -Werror builds.
if(MSVC)
  target_compile_options(pp_http PRIVATE /W0 /WX-)
else()
  target_compile_options(pp_http PRIVATE -Wno-error -w)
endif()
