find_program(NODE_EXECUTABLE node REQUIRED)

execute_process(
  COMMAND ${NODE_EXECUTABLE} -p "require('node-addon-api').include"
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE NODE_ADDON_API_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE NODE_ADDON_API_RESULT
  ERROR_VARIABLE NODE_ADDON_API_ERROR
)

if(NODE_ADDON_API_RESULT)
  message(FATAL_ERROR "node-addon-api not found in node-addon/. Run: cd node-addon && npm install")
endif()

string(REPLACE "\"" "" NODE_ADDON_API_INCLUDE "${NODE_ADDON_API_INCLUDE}")

execute_process(
  COMMAND ${NODE_EXECUTABLE} -p "process.config.variables.nodedir || ''"
  OUTPUT_VARIABLE NODE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
  COMMAND ${NODE_EXECUTABLE} -p "require('path').dirname(require('path').dirname(process.execPath))"
  OUTPUT_VARIABLE NODE_PREFIX_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NODE_DIR STREQUAL "")
  set(NODE_INCLUDE_DIR "${NODE_PREFIX_DIR}/include/node")
else()
  set(NODE_INCLUDE_DIR "${NODE_DIR}/include/node")
endif()

if(NOT EXISTS "${NODE_INCLUDE_DIR}")
  set(NODE_INCLUDE_DIR "${NODE_PREFIX_DIR}/include/node")
endif()

if(NOT EXISTS "${NODE_INCLUDE_DIR}")
  set(NODE_INCLUDE_DIR "/usr/include/node")
endif()

add_library(pp_client_node MODULE
  AddonUtils.cpp
  ClientWrapper.cpp
  addon.cpp
)

set_target_properties(pp_client_node PROPERTIES
  PREFIX ""
  SUFFIX ".node"
)

target_compile_definitions(pp_client_node PRIVATE
  NAPI_CPP_EXCEPTIONS
)

target_include_directories(pp_client_node PRIVATE
  ${NODE_ADDON_API_INCLUDE}
  ${NODE_INCLUDE_DIR}
)

target_link_libraries(pp_client_node PRIVATE
  pp_client
)
