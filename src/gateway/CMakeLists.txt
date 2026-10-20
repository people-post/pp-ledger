# The verifying gateway (projects/finality FN004): follows a chain from a pinned genesis, as a client
# verifies it; pp-http serves it on /v1/.
add_library(pp_gateway STATIC
    Gateway.cpp
)

target_link_libraries(pp_gateway PUBLIC
    pp_chain
    pp_client
    pp_ledger_common
)

if(PP_LEDGER_BUILD_TESTS)
  add_subdirectory(test)
endif()
