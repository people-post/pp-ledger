cmake_minimum_required(VERSION 3.14)

# Consensus Library
project(consensus VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(CONSENSUS_SOURCES
    Ouroboros.cpp
    SlotLeaderSelection.cpp
    EpochManager.cpp
)

# Header files
set(CONSENSUS_HEADERS
    Ouroboros.h
    SlotLeaderSelection.h
    EpochManager.h
)

# Create the library
add_library(consensus STATIC ${CONSENSUS_SOURCES} ${CONSENSUS_HEADERS})

# Include directories
target_include_directories(consensus
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/lib
        ${CMAKE_SOURCE_DIR}/interface
)

# Link with lib (for Module, Logger, ResultOrError)
target_link_libraries(consensus
    PUBLIC
        ppledger_lib
)

# Set properties
set_target_properties(consensus PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${CONSENSUS_HEADERS}"
)

# Installation rules
install(TARGETS consensus
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include/consensus
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

message(STATUS "Consensus library configured")
