#!/usr/bin/env bash
# Smoke network lifecycle: beacon → relay → miners (+ optional pp-http).
# Ports default to 8617+ (see pp_ledger_smoke_lib.sh).
#
# Usage:
#   ./scripts/test/pp_ledger_network.sh up|stop|clear|status
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

usage() {
  cat <<EOF
Usage: $(basename "$0") <up|stop|clear|status>

  up      Init short-slot beacon, start beacon→relay→miners (+ http if built)
  stop    Stop processes (keep data)
  clear   Stop and remove \$PP_LEDGER_SMOKE_DIR (default build/test-smoke)
  status  Process liveness + best-effort pp-client reachability

Environment:
  PP_LEDGER_BUILD_DIR  PP_LEDGER_SMOKE_DIR  PP_LEDGER_SMOKE_MINERS
  PP_LEDGER_SMOKE_BEACON_PORT / RELAY_PORT / MINER_BASE_PORT / HTTP_PORT
EOF
}

cmd=${1:-}
case "$cmd" in
  up) network_up ;;
  stop) stop_network ;;
  clear|clean) clear_network ;;
  status) network_status ;;
  -h|--help|help) usage ;;
  *)
    usage >&2
    exit 2
    ;;
esac
