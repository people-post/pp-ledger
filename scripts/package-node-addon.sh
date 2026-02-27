#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if git -C "$SCRIPT_DIR" rev-parse --show-toplevel &>/dev/null; then
  REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
else
  REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
fi
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

# Version: first argument or env VERSION (required for archive name)
if [[ $# -gt 0 && "$1" != --* ]]; then
  VERSION="$1"
  shift
else
  VERSION="${VERSION:-}"
fi
if [[ -z "$VERSION" ]]; then
  echo "Usage: $0 <version> [platform]" >&2
  echo "  or set VERSION (and optionally PLATFORM) in the environment" >&2
  exit 1
fi

# Platform: second argument or env PLATFORM, default from uname
if [[ $# -gt 0 && "$1" != --* ]]; then
  PLATFORM="$1"
  shift
else
  PLATFORM="${PLATFORM:-}"
fi
if [[ -z "$PLATFORM" ]]; then
  OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64|amd64) ARCH=x64 ;;
    aarch64|arm64) ARCH=arm64 ;;
  esac
  PLATFORM="${OS}-${ARCH}"
fi

ADDON_BINARY="$BUILD_DIR/node-addon/pp_client_node.node"
NODE_ADDON_SOURCE="$REPO_ROOT/node-addon"
STAGING_DIR="$BUILD_DIR/release-package"
ARCHIVE_NAME="pp-ledger-node-addon-${VERSION}-${PLATFORM}.tar.gz"
OUTPUT_ARCHIVE="$BUILD_DIR/$ARCHIVE_NAME"

if [[ ! -f "$ADDON_BINARY" ]]; then
  echo "Addon binary not found: $ADDON_BINARY" >&2
  echo "Run the build with --node-addon first (e.g. ./scripts/ci-build.sh --node-addon)" >&2
  exit 1
fi

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

cp "$ADDON_BINARY" "$STAGING_DIR/pp_client_node.node"
cp "$NODE_ADDON_SOURCE/index.js" "$STAGING_DIR/"
cp "$NODE_ADDON_SOURCE/index.d.ts" "$STAGING_DIR/"
cp "$NODE_ADDON_SOURCE/package.json" "$STAGING_DIR/"

# Use tar czf (no -v) so stdout contains only the path we echo below, for CI capture
tar czf "$OUTPUT_ARCHIVE" -C "$STAGING_DIR" .
# Print path only (no newline) so CI capture gives a single valid path
printf '%s' "$OUTPUT_ARCHIVE"
