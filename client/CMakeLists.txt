# Client library
add_library(pp_client STATIC
    Client.cpp
    Client.h
)

target_include_directories(pp_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pp_client PUBLIC
    pp_lib
    pp_network
)
