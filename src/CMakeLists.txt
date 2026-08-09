# Source tree: libraries, servers, apps, and optional node addon.

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

if(BUILD_NODE_ADDON)
  add_subdirectory(node-addon)
endif()
