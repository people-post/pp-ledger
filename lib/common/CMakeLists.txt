# Lib library
add_library(pp_lib STATIC
    Crypto.cpp
    Crypto.h
    Logger.cpp
    Logger.h
    io/Json.cpp
    io/Json.h
    Meta.cpp
    Meta.h
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

# Public include directories - automatically propagated to dependents
target_include_directories(pp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)

# Find Libsodium (required for cryptographic operations in Utilities.cpp)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

# Link dependencies with PUBLIC to automatically propagate to dependents
# This ensures any target linking to pp_lib automatically gets:
# - pp_lib headers (via include directories)
# - Libsodium library and headers
target_link_libraries(pp_lib PUBLIC
    ${SODIUM_LIBRARIES}
    nlohmann_json::nlohmann_json
)

# Add Libsodium include directories
target_include_directories(pp_lib PUBLIC
    ${SODIUM_INCLUDE_DIRS}
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
