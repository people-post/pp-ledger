# Ledger library
add_library(pp_ledger STATIC
    Account.cpp
    Ledger.cpp
    DirStore.cpp
    FileStore.cpp
    FileDirStore.cpp
    VolumeStore.cpp
)

target_include_directories(pp_ledger PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

# Link to pp_ledger_common - automatically gets all pp_ledger_common dependencies:
# - pp_ledger_common headers and libraries
# - pp-cpp-common (Value/Meta/JSON, Serialize, …)
# - pp-cpp-crypto / sodium
target_link_libraries(pp_ledger PUBLIC
    pp_ledger_common
)

# Add tests subdirectory if testing is enabled
if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
endif()
