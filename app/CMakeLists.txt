# Application executable
add_executable(pp-ledger
    main.cpp
)

target_link_libraries(pp-ledger PRIVATE
    ppledger_lib
    ppledger_client
    ppledger_server
)
