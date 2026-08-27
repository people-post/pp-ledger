# Fetch / add people-post/pp-cpp-common (target pp_common).
#
# Prefer a local sibling checkout when present (ppweb3 workspace layout).
# Otherwise pin a release tag from that repo's main line (PP_CPP_COMMON_GIT_TAG).
# When embedded (e.g. pp-browser already defined pp_common), this is a no-op.

if(TARGET pp_common)
  return()
endif()

include(FetchContent)

set(PP_CPP_COMMON_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-common (overrides FetchContent)")
set(PP_CPP_COMMON_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-common.git"
  CACHE STRING "Git remote for pp-cpp-common")
set(PP_CPP_COMMON_GIT_TAG "v0.2.0"
  CACHE STRING "Release tag on pp-cpp-common main (not a branch name)")

set(PP_COMMON_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-common tests" FORCE)

set(_pp_common_sibling "${CMAKE_SOURCE_DIR}/../pp-cpp-common")
if(PP_CPP_COMMON_SOURCE_DIR AND EXISTS "${PP_CPP_COMMON_SOURCE_DIR}/CMakeLists.txt")
  set(_pp_common_src "${PP_CPP_COMMON_SOURCE_DIR}")
elseif(EXISTS "${_pp_common_sibling}/CMakeLists.txt")
  set(_pp_common_src "${_pp_common_sibling}")
  message(STATUS "pp-ledger: using sibling pp-cpp-common at ${_pp_common_src}")
else()
  if(PP_CPP_COMMON_SOURCE_DIR)
    message(WARNING "PP_CPP_COMMON_SOURCE_DIR=${PP_CPP_COMMON_SOURCE_DIR} is missing; falling back to FetchContent")
  endif()
  set(_pp_common_src "")
endif()

if(_pp_common_src)
  add_subdirectory("${_pp_common_src}"
                   "${CMAKE_BINARY_DIR}/_deps/pp_cpp_common-build" EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(
    pp_cpp_common
    GIT_REPOSITORY ${PP_CPP_COMMON_GIT_REPOSITORY}
    GIT_TAG ${PP_CPP_COMMON_GIT_TAG}
  )
  FetchContent_MakeAvailable(pp_cpp_common)
endif()

if(NOT TARGET pp_common)
  message(FATAL_ERROR "pp-cpp-common did not define target pp_common")
endif()

unset(_pp_common_sibling)
unset(_pp_common_src)
