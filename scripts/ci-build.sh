#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if git -C "$SCRIPT_DIR" rev-parse --show-toplevel &>/dev/null; then
  REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
else
  REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
fi
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

DO_BUILD=1
DO_TEST=
WITH_TESTS=

while [[ $# -gt 0 ]]; do
  case "$1" in
    # Configure + build with tests enabled, then run ctest (local convenience).
    --test)
      WITH_TESTS=1
      DO_TEST=1
      ;;
    # Configure + build only (optionally with test targets).
    --build)
      DO_BUILD=1
      DO_TEST=
      ;;
    --with-tests)
      WITH_TESTS=1
      ;;
    # Run ctest against an existing build directory.
    --run-tests)
      DO_BUILD=
      DO_TEST=1
      ;;
    *)
      echo "Usage: $0 [--build] [--with-tests] [--run-tests] [--test]" >&2
      echo "  --build        Configure and build (default)." >&2
      echo "  --with-tests   Enable PP_LEDGER_BUILD_TESTS=ON during configure." >&2
      echo "  --run-tests    Run ctest only (no configure/build)." >&2
      echo "  --test         Shorthand: --build --with-tests --run-tests." >&2
      exit 1
      ;;
  esac
  shift
done

# Default action when no flags are given: build without running tests.
if [[ -z "$DO_BUILD" && -z "$DO_TEST" ]]; then
  DO_BUILD=1
fi

if [[ -n "$DO_BUILD" ]]; then
  mkdir -p "$BUILD_DIR"

  CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=Release
    -S "$REPO_ROOT"
    -B "$BUILD_DIR"
  )

  # Ninja on Windows, and in CI wherever it is installed: it keeps every core busy. (Not for
  # local runs elsewhere: an existing build directory keeps the generator it was made with.)
  if [[ "${RUNNER_OS:-}" == "Windows" ]] || { [[ -n "${CI:-}" ]] && command -v ninja >/dev/null 2>&1; }; then
    CMAKE_ARGS+=(-G Ninja)
  fi

  if [[ -n "$WITH_TESTS" ]]; then
    CMAKE_ARGS+=(-DPP_LEDGER_BUILD_TESTS=ON)
  fi

  # Note: avoid empty "${arr[@]}" under `set -u` — bash < 4.4 (macOS) treats it
  # as unbound. Fold optional flags into always-nonempty argument lists.
  BUILD_ARGS=("$BUILD_DIR")
  if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
    BUILD_ARGS+=(--config Release)
  fi

  cmake "${CMAKE_ARGS[@]}"
  cmake --build "${BUILD_ARGS[@]}" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
fi

if [[ -n "$DO_TEST" ]]; then
  # ctest uses -C/--build-config for multi-config generators; --config is only
  # valid for `cmake --build`.
  # Test cases run in parallel processes; tests take private temp paths (src/testing/TestTempPath.h).
  # Two at a time by default (CTEST_PARALLEL_LEVEL): some tests time slots and syncs, and stay
  # steady on shared runners however many cores compile.
  CTEST_ARGS=(--test-dir "$BUILD_DIR" --output-on-failure --parallel "${CTEST_PARALLEL_LEVEL:-2}")
  if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
    CTEST_ARGS+=(-C Release)
  fi
  ctest "${CTEST_ARGS[@]}"
fi
