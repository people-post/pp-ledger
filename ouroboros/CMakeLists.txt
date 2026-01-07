cmake_minimum_required(VERSION 3.14)

# Ouroboros Consensus Library
project(ouroboros VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(OUROBOROS_SOURCES
    OuroborosConsensus.cpp
    SlotLeaderSelection.cpp
    EpochManager.cpp
)

# Header files
set(OUROBOROS_HEADERS
    IBlock.h
    IBlockChain.h
    OuroborosConsensus.h
    SlotLeaderSelection.h
    EpochManager.h
)

# Create the library
add_library(ouroboros STATIC ${OUROBOROS_SOURCES} ${OUROBOROS_HEADERS})

# Include directories
target_include_directories(ouroboros
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/lib
)

# Link with lib (for Module, Logger, ResultOrError)
target_link_libraries(ouroboros
    PUBLIC
        ppledger_lib
)

# Set properties
set_target_properties(ouroboros PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${OUROBOROS_HEADERS}"
)

# Installation rules
install(TARGETS ouroboros
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include/ouroboros
)

message(STATUS "Ouroboros consensus library configured")
