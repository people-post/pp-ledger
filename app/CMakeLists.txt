# Server executable
add_executable(pp-server
    server.cpp
)

target_link_libraries(pp-server PRIVATE
    pp_lib
    pp_server
)

# Client executable
add_executable(pp-client
    client.cpp
)

target_link_libraries(pp-client PRIVATE
    pp_lib
    pp_client
    pp_ledger
)

# Beacon executable
add_executable(pp-beacon
    beacon.cpp
)

target_link_libraries(pp-beacon PRIVATE
    pp_lib
    pp_server
)
