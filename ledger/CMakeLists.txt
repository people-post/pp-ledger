# Ledger library
add_library(ppledger_ledger STATIC
    BlockChain.cpp
    BlockChain.h
    Ledger.cpp
    Ledger.h
    Wallet.cpp
    Wallet.h
    BlockFile.cpp
    BlockFile.h
    BlockDir.cpp
    BlockDir.h
)

target_include_directories(ppledger_ledger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ppledger_ledger PUBLIC
    ppledger_lib
    crypto
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
