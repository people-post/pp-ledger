# Ledger library
add_library(pp_ledger STATIC
    BlockChain.cpp
    Wallet.cpp
    Wallet.h
    Ledger.cpp
    Ledger.h
    DirStore.cpp
    DirStore.h
    FileStore.cpp
    FileStore.h
    FileDirStore.cpp
    FileDirStore.h
    DirDirStore.cpp
    DirDirStore.h
)

target_include_directories(pp_ledger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link to pp_lib - automatically gets all pp_lib dependencies:
# - pp_lib headers and libraries
# - OpenSSL (via pp_lib)
# - nlohmann/json headers (via pp_lib)
target_link_libraries(pp_ledger PUBLIC
    pp_lib
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
