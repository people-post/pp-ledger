# Source tree: libraries, servers, and apps.

if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -Werror)
endif()

add_subdirectory(lib)
add_subdirectory(consensus)
add_subdirectory(network)
add_subdirectory(client)
add_subdirectory(ledger)
add_subdirectory(chain)
add_subdirectory(server)

if(PP_LEDGER_BUILD_HTTP)
  add_subdirectory(lib/http)
endif()

if(PP_LEDGER_BUILD_APPS)
  add_subdirectory(app)
endif()
