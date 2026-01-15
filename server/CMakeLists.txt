# Server library
add_library(pp_server STATIC
    Server.cpp
    Server.h
)

target_include_directories(pp_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/network
)

# Server requires network library
target_link_libraries(pp_server PUBLIC
    pp_lib
    pp_ledger
    pp_client
    pp_consensus
    pp_network
)

message(STATUS "Server configured with TCP network support")
