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

# Public include directories - automatically propagated to dependents
target_include_directories(pp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
)

# Find OpenSSL (required for sha256 in Utilities.cpp)
find_package(OpenSSL REQUIRED)

# Link dependencies with PUBLIC to automatically propagate to dependents
# This ensures any target linking to pp_lib automatically gets:
# - pp_lib headers (via include directories)
# - OpenSSL library and headers (via OpenSSL::Crypto target)
# - nlohmann/json headers (via include directories)
target_link_libraries(pp_lib PUBLIC
    OpenSSL::Crypto
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
