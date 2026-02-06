# Client executable
add_executable(pp-client
    client.cpp
)

target_link_libraries(pp-client PRIVATE
    pp_lib
    pp_client
    pp_ledger
    CLI11::CLI11
)

# Beacon executable
add_executable(pp-beacon
    beacon.cpp
)

target_link_libraries(pp-beacon PRIVATE
    pp_lib
    pp_server
    CLI11::CLI11
)

# Relay executable
add_executable(pp-relay
    relay.cpp
)

target_link_libraries(pp-relay PRIVATE
    pp_lib
    pp_server
    CLI11::CLI11
)

# Miner executable
add_executable(pp-miner
    miner.cpp
)

target_link_libraries(pp-miner PRIVATE
    pp_lib
    pp_server
    CLI11::CLI11
)
