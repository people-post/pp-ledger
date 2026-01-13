# Server library
add_library(ppledger_server STATIC
    Server.cpp
    Server.h
)

target_include_directories(ppledger_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ppledger_server PUBLIC
    ppledger_lib
    ppledger_ledger
    consensus
)
# Server demo executable
add_executable(server-demo ServerDemo.cpp)
target_link_libraries(server-demo PUBLIC ppledger_server)