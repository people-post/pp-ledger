# Operation files (docs/contracts/OPERATION_FILE.md): no network code — an offline signer links this
# library alone.
add_library(pp_operation STATIC
    Operation.cpp
    Operation.h
)
target_include_directories(pp_operation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PP_SRC_DIR})
target_link_libraries(pp_operation PUBLIC pp_chain_types pp_ledger pp_ledger_common)

if(PP_LEDGER_BUILD_TESTS)
    add_subdirectory(test)
endif()
