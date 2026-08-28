# Keep historical artifact path: build/app/<binary>
set(PP_APP_OUTPUT_DIR "${CMAKE_BINARY_DIR}/app")

# Client executable
add_executable(pp-client
    client.cpp
)

target_link_libraries(pp-client PRIVATE
    pp_ledger_common
    pp_client
    pp_ledger
    CLI11::CLI11
)
set_target_properties(pp-client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PP_APP_OUTPUT_DIR}")

# Beacon executable
add_executable(pp-beacon
    beacon.cpp
)

target_link_libraries(pp-beacon PRIVATE
    pp_ledger_common
    pp_server
    CLI11::CLI11
)
set_target_properties(pp-beacon PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PP_APP_OUTPUT_DIR}")

# Relay executable
add_executable(pp-relay
    relay.cpp
)

target_link_libraries(pp-relay PRIVATE
    pp_ledger_common
    pp_server
    CLI11::CLI11
)
set_target_properties(pp-relay PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PP_APP_OUTPUT_DIR}")

# Miner executable
add_executable(pp-miner
    miner.cpp
)

target_link_libraries(pp-miner PRIVATE
    pp_ledger_common
    pp_server
    CLI11::CLI11
)
set_target_properties(pp-miner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PP_APP_OUTPUT_DIR}")

# HTTP API server (client interfaces over HTTP, using cpp-httplib)
if(BUILD_HTTP)
  add_executable(pp-http
      http.cpp
  )

  target_include_directories(pp-http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(pp-http PRIVATE
      pp_ledger_common
      pp_client
      pp_ledger
      pp_http
      CLI11::CLI11
  )
  set_target_properties(pp-http PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PP_APP_OUTPUT_DIR}")
endif()
