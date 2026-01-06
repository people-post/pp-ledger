# Lib library
add_library(ppledger_lib STATIC
    Lib.cpp
    Lib.h
)

target_include_directories(ppledger_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
