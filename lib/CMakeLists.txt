# Lib library
add_library(pp_lib STATIC
    Logger.cpp
    Logger.h
    Module.cpp
    Module.h
    ResultOrError.hpp
    Serialize.hpp
    Service.cpp
    Service.h
    BinaryPack.hpp
    Utilities.cpp
    Utilities.h
)

target_include_directories(pp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add nlohmann/json dependency for JSON utilities
target_include_directories(pp_lib PUBLIC
    ${nlohmann_json_SOURCE_DIR}/include
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
