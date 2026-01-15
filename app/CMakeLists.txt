# Server executable
add_executable(pp-ledger-server
    server.cpp
)

target_link_libraries(pp-ledger-server PRIVATE
    ppledger_lib
    ppledger_server
)

# Client executable
add_executable(pp-ledger-client
    client.cpp
)

target_link_libraries(pp-ledger-client PRIVATE
    ppledger_lib
    ppledger_client
    ppledger_ledger
)
