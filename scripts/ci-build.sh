#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if git -C "$SCRIPT_DIR" rev-parse --show-toplevel &>/dev/null; then
  REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
else
  REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
fi
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

NODE_ADDON=
RUN_TESTS=

while [[ $# -gt 0 ]]; do
  case "$1" in
    --node-addon) NODE_ADDON=ON ;;
    --test)       RUN_TESTS=1 ;;
    *)
      echo "Usage: $0 [--node-addon] [--test]" >&2
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
if [[ -n "$NODE_ADDON" ]]; then
  CMAKE_ARGS+=(-DBUILD_NODE_ADDON=ON)
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$(nproc)"

if [[ -n "$RUN_TESTS" ]]; then
  ctest --test-dir "$BUILD_DIR" --output-on-failure
fi
