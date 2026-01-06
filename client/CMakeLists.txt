# Client library
add_library(ppledger_client STATIC
    Client.cpp
    Client.h
)

target_include_directories(ppledger_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ppledger_client PUBLIC
    ppledger_lib
)
