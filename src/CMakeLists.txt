# Source tree: libraries, servers, and apps.

# Enable common warnings and treat them as errors for project code.
# Vendored targets (libsodium, CLI11, httplib) opt out via -Wno-error.
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
if(BUILD_HTTP)
  add_subdirectory(lib/http)
endif()
add_subdirectory(app)
