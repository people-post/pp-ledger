# Fetch people-post/pp-cpp-crypto (sodium + ML-KEM/ML-DSA + pp_crypto).
#
# Pin a release tag (PP_CPP_CRYPTO_GIT_TAG). When embedded (e.g. pp-browser
# already defined pp_crypto), this is a no-op.
#
# Optional override: PP_CPP_CRYPTO_SOURCE_DIR (local checkout path).

if(TARGET pp_crypto AND TARGET sodium)
  return()
endif()

include(FetchContent)

set(PP_CPP_CRYPTO_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-crypto (overrides FetchContent)")
set(PP_CPP_CRYPTO_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-crypto.git"
  CACHE STRING "Git remote for pp-cpp-crypto")
set(PP_CPP_CRYPTO_GIT_TAG "v0.1.0"
  CACHE STRING "Release tag on pp-cpp-crypto main (not a branch name)")

set(PP_CRYPTO_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-crypto tests" FORCE)

if(PP_CPP_CRYPTO_SOURCE_DIR AND EXISTS "${PP_CPP_CRYPTO_SOURCE_DIR}/CMakeLists.txt")
  add_subdirectory("${PP_CPP_CRYPTO_SOURCE_DIR}"
                   "${CMAKE_BINARY_DIR}/_deps/pp_cpp_crypto-build" EXCLUDE_FROM_ALL)
else()
  if(PP_CPP_CRYPTO_SOURCE_DIR)
    message(WARNING "PP_CPP_CRYPTO_SOURCE_DIR=${PP_CPP_CRYPTO_SOURCE_DIR} is missing; falling back to FetchContent")
  endif()
  FetchContent_Declare(
    pp_cpp_crypto
    GIT_REPOSITORY ${PP_CPP_CRYPTO_GIT_REPOSITORY}
    GIT_TAG ${PP_CPP_CRYPTO_GIT_TAG}
  )
  FetchContent_MakeAvailable(pp_cpp_crypto)
endif()

if(NOT TARGET pp_crypto)
  message(FATAL_ERROR "pp-cpp-crypto did not define target pp_crypto")
endif()
if(NOT TARGET sodium)
  message(FATAL_ERROR "pp-cpp-crypto did not define target sodium")
endif()
if(NOT TARGET mldsa_native OR NOT TARGET mlkem_native)
  message(FATAL_ERROR "pp-cpp-crypto did not define mldsa_native / mlkem_native")
endif()
