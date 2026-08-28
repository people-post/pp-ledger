# jech/dht (BitTorrent DHT) - C library with user-provided hooks
enable_language(C)

set(DHT_SOURCES
    dht.c
    dht_user_common.c
)

if(WIN32)
  list(APPEND DHT_SOURCES dht_user_win32.c)
else()
  list(APPEND DHT_SOURCES dht_user_posix.c)
endif()

add_library(pp_dht STATIC ${DHT_SOURCES})

target_include_directories(pp_dht
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pp_dht
    PUBLIC
        sodium::sodium
)

if(WIN32)
  target_link_libraries(pp_dht PUBLIC ws2_32)
  target_compile_definitions(pp_dht PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

set_target_properties(pp_dht PROPERTIES
    C_STANDARD 99
)
