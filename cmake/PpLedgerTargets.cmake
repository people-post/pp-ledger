# Define pp:: namespaced ALIAS targets for exported libraries.
function(pp_ledger_define_aliases)
  set(_pairs
      pp_ledger_common:ledger_common
      pp_consensus:consensus
      pp_network:network
      pp_ledger:ledger
      pp_client:client
      pp_chain:chain
      pp_server:server)
  foreach(pair IN LISTS _pairs)
    string(REPLACE ":" ";" _parts "${pair}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _alias)
    if(TARGET ${_target} AND NOT TARGET pp::${_alias})
      add_library(pp::${_alias} ALIAS ${_target})
    endif()
  endforeach()
endfunction()
