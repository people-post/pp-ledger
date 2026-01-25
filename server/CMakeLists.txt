# Server library
add_library(pp_server STATIC
    Validator.cpp
    Validator.h
    ValidatorServer.cpp
    ValidatorServer.h
    BeaconServer.cpp
    BeaconServer.h
    Beacon.cpp
    Beacon.h
    MinerServer.cpp
    MinerServer.h
    Miner.cpp
    Miner.h
)

target_include_directories(pp_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/network
)

# Add nlohmann/json include directory
target_include_directories(pp_server PRIVATE
    ${nlohmann_json_SOURCE_DIR}/include
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

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
