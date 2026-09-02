# Fetch people-post/pp-cpp-amp (ADP L1, MSH L2, channel L3, link).
#
# Prefer a local sibling checkout when present. Override with PP_CPP_AMP_SOURCE_DIR.

include(FetchContent)

set(PP_CPP_AMP_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-amp (overrides FetchContent)")
set(PP_CPP_AMP_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-amp.git"
  CACHE STRING "Git remote for pp-cpp-amp")
set(PP_CPP_AMP_GIT_TAG "main"
  CACHE STRING "Release tag on pp-cpp-amp main (not a branch name)")

set(PP_AMP_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-amp tests" FORCE)

set(_pp_amp_sibling "${CMAKE_SOURCE_DIR}/../pp-cpp-amp")
if(PP_CPP_AMP_SOURCE_DIR)
  set(_pp_amp_src "${PP_CPP_AMP_SOURCE_DIR}")
elseif(EXISTS "${_pp_amp_sibling}/CMakeLists.txt")
  set(_pp_amp_src "${_pp_amp_sibling}")
  message(STATUS "pp-ledger: using sibling pp-cpp-amp at ${_pp_amp_src}")
else()
  set(_pp_amp_src "")
endif()

if(NOT TARGET pp_amp_l1)
  if(_pp_amp_src)
    add_subdirectory("${_pp_amp_src}"
                     "${CMAKE_BINARY_DIR}/_deps/pp_cpp_amp-build" EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(
      pp_cpp_amp
      GIT_REPOSITORY ${PP_CPP_AMP_GIT_REPOSITORY}
      GIT_TAG ${PP_CPP_AMP_GIT_TAG}
    )
    FetchContent_MakeAvailable(pp_cpp_amp)
  endif()
endif()

foreach(_layer pp_amp_l1 pp_amp_l2 pp_amp_l3 pp_amp_link)
  if(NOT TARGET ${_layer})
    message(FATAL_ERROR "pp-cpp-amp did not define target ${_layer}")
  endif()
endforeach()
