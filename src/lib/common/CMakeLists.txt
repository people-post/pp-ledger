# Lib library — ledger-specific common (Crypto, Service, Utilities, …).
# Shared Logger / Module / ResultOrError / Serialize / Value+JSON come from
# pp-cpp-common (include as common/…).
add_library(pp_ledger_common STATIC
    Crypto.cpp
    FileSync.cpp
    Service.cpp
    Utilities.cpp
)

# Public include directories - automatically propagated to dependents
target_include_directories(pp_ledger_common PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(pp_ledger_common PUBLIC
    pp_common
    pp_crypto
    sodium::sodium
)

if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
endif()
