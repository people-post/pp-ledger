#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if git -C "$SCRIPT_DIR" rev-parse --show-toplevel &>/dev/null; then
  REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
else
  REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
fi
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

RUN_TESTS=

while [[ $# -gt 0 ]]; do
  case "$1" in
    --test) RUN_TESTS=1 ;;
    *)
      echo "Usage: $0 [--test]" >&2
      exit 1
      ;;
  esac
  shift
done

mkdir -p "$BUILD_DIR"

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -S "$REPO_ROOT"
  -B "$BUILD_DIR"
)

GENERATOR=()
BUILD_CONFIG=()
if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
  GENERATOR=(-G Ninja)
  BUILD_CONFIG=(--config Release)
fi

if [[ -n "$RUN_TESTS" ]]; then
  CMAKE_ARGS+=(-DPP_LEDGER_BUILD_TESTS=ON)
fi

cmake "${CMAKE_ARGS[@]}" "${GENERATOR[@]}"
cmake --build "$BUILD_DIR" "${BUILD_CONFIG[@]}" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

if [[ -n "$RUN_TESTS" ]]; then
  ctest --test-dir "$BUILD_DIR" "${BUILD_CONFIG[@]}" --output-on-failure
fi
