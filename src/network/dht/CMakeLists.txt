# jech/dht (BitTorrent DHT) - C library with user-provided hooks
enable_language(C)

add_library(pp_dht STATIC
    dht.c
    dht_user.c
)

target_include_directories(pp_dht
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# User implementations use libsodium (dht_random_bytes) and need socket headers
target_link_libraries(pp_dht
    PUBLIC
        ${SODIUM_LIBRARIES}
)

target_include_directories(pp_dht
    PUBLIC
        ${SODIUM_INCLUDE_DIRS}
)

# C only: no C++ standard
set_target_properties(pp_dht PROPERTIES
    C_STANDARD 99
)
