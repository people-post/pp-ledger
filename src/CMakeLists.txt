# Source tree: libraries, servers, and apps.

# Treat warnings as errors for project code (vendored targets under lib/ opt out).
add_compile_options(-Werror)

add_subdirectory(lib)
add_subdirectory(consensus)
add_subdirectory(network)
add_subdirectory(client)
add_subdirectory(ledger)
add_subdirectory(chain)
add_subdirectory(server)
if(BUILD_HTTP)
  add_subdirectory(lib/http)
endif()
add_subdirectory(app)
