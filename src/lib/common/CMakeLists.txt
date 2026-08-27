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
    PqCryptoBridge.cpp
    PqCryptoBridge.h
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
    ${PP_SRC_DIR}
)

# Link dependencies with PUBLIC to automatically propagate to dependents:
# - vendored nlohmann/json headers
# - pp-cpp-crypto (ML-DSA/ML-KEM + libsodium)
target_link_libraries(pp_lib PUBLIC
    nlohmann_json::nlohmann_json
    pp_crypto
    sodium::sodium
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
