# Lib library — ledger-specific common (Crypto, Service, Utilities, …).
# Shared Logger / Module / ResultOrError / Serialize / BinaryPack / Value+JSON
# come from pp-cpp-common via thin shims under this directory.
add_library(pp_ledger_common STATIC
    Crypto.cpp
    Crypto.h
    Logger.h
    Module.h
    FiFoMap.h
    Value.h
    Meta.h
    io/Json.h
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
target_include_directories(pp_ledger_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PP_SRC_DIR}
)

# Link dependencies with PUBLIC to automatically propagate to dependents:
# - pp-cpp-common (Logger, Module, Roe, Serialize, …)
# - pp-cpp-crypto (ML-DSA/ML-KEM + libsodium)
target_link_libraries(pp_ledger_common PUBLIC
    pp_common
    pp_crypto
    sodium::sodium
)

# Add tests subdirectory if testing is enabled
if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
endif()
