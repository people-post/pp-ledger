# Server library
add_library(ppledger_server STATIC
    Server.cpp
    Server.h
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

target_include_directories(ppledger_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ppledger_server PUBLIC
    ppledger_lib
    crypto
)
