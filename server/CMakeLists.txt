# Server library
add_library(ppledger_server STATIC
    Server.cpp
    Server.h
)

target_include_directories(ppledger_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/network
)

# Server requires network library
target_link_libraries(ppledger_server PUBLIC
    ppledger_lib
    ppledger_ledger
    consensus
    network
)

message(STATUS "Server configured with TCP network support")
