# Lib library — ledger-specific common (Crypto, Service, Utilities, …).
# Shared Logger / Module / ResultOrError / Serialize / Value+JSON come from
# pp-cpp-common (include as common/…).
add_library(pp_ledger_common STATIC
    Crypto.cpp
    Crypto.h
    Meta.h
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

target_link_libraries(pp_ledger_common PUBLIC
    pp_common
    pp_crypto
    sodium::sodium
)

if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
endif()
