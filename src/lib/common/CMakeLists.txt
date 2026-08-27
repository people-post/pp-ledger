# Lib library — ledger-specific common (Value/Meta, Crypto, Service, Utilities, …).
# Shared Logger / Module / ResultOrError / Serialize / BinaryPack come from
# pp-cpp-common via thin shims under this directory.
add_library(pp_lib STATIC
    Crypto.cpp
    Crypto.h
    Logger.h
    Module.h
    io/Json.cpp
    io/Json.h
    FiFoMap.h
    Value.cpp
    Value.h
    Meta.h
    ResultOrError.hpp
    Serialize.hpp
    BinaryPack.hpp
    Service.cpp
    Service.h
    ThreadSafeQueue.hpp
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
# - pp-cpp-common (Logger, Module, Roe, Serialize, …)
# - pp-cpp-crypto (ML-DSA/ML-KEM + libsodium)
target_link_libraries(pp_lib PUBLIC
    nlohmann_json::nlohmann_json
    pp_common
    pp_crypto
    sodium::sodium
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
