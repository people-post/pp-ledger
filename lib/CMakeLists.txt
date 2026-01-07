# Lib library
add_library(ppledger_lib STATIC
    Lib.cpp
    Lib.h
    Logger.cpp
    Logger.h
    ResultOrError.hpp
    TcpClient.cpp
    TcpClient.h
    TcpServer.cpp
    TcpServer.h
)

target_include_directories(ppledger_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
