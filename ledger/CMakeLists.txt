# Ledger library
add_library(pp_ledger STATIC
    Block.cpp
    Block.h
    BlockChain.cpp
    BlockChain.h
    Wallet.cpp
    Wallet.h
    BlockStore.hpp
    DirStore.h
    FileStore.cpp
    FileStore.h
    FileDirStore.cpp
    FileDirStore.h
    DirDirStore.cpp
    DirDirStore.h
    BlockDir.cpp
    BlockDir.h
)

target_include_directories(pp_ledger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pp_ledger PUBLIC
    pp_lib
    crypto
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
