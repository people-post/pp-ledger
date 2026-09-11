#!/usr/bin/env bash
# Compatibility wrapper → scripts/test/pp_ledger_network.sh (smoke profile, ports 8617+).
# Prefer: ./scripts/test/pp_ledger_local_test.sh
#
# start|restart → up; stop|clear|status delegated. logs still work against smoke data dir.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NETWORK="${ROOT}/scripts/test/pp_ledger_network.sh"
# shellcheck source=scripts/test/pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

usage() {
  cat <<EOF
Usage: $0 COMMAND

Commands:
  start|up     Start smoke network (beacon→relay→miners)
  stop         Stop smoke network
  restart      Stop then start
  status       Process + RPC status
  clear        Stop and remove smoke data
  logs         Show logs (-b beacon | -m N miner | -H http | -f follow)

Legacy dogfood ports (8517) are replaced by the smoke profile (8617+).
See docs/ops/TEST_STRATEGY.md and scripts/test/pp_ledger_local_test.sh.
EOF
}

show_logs() {
  local show_beacon=false show_http=false follow=false miner_num=""
  shift || true
  while getopts "bm:Hf" opt; do
    case "$opt" in
      b) show_beacon=true ;;
      m) miner_num=$OPTARG ;;
      H) show_http=true ;;
      f) follow=true ;;
      *) usage; exit 1 ;;
    esac
  done
  local path=""
  if [[ "$show_beacon" == true ]]; then
    path="${TEST_DIR}/beacon/console.log"
  elif [[ -n "$miner_num" ]]; then
    path="${TEST_DIR}/miner${miner_num}/console.log"
  elif [[ "$show_http" == true ]]; then
    path="${TEST_DIR}/http/console.log"
  else
    echo "Specify -b, -m NUM, or -H" >&2
    exit 1
  fi
  [[ -f "$path" ]] || { echo "Log not found: $path" >&2; exit 1; }
  if [[ "$follow" == true ]]; then
    tail -f "$path"
  else
    cat "$path"
  fi
}

cmd=${1:-}
case "$cmd" in
  start|up) bash "$NETWORK" up ;;
  stop) bash "$NETWORK" stop ;;
  restart)
    bash "$NETWORK" stop || true
    bash "$NETWORK" up
    ;;
  status) bash "$NETWORK" status ;;
  clear|clean) bash "$NETWORK" clear ;;
  logs) show_logs "$@" ;;
  -h|--help|help|"") usage ;;
  *)
    echo "Unknown command: $cmd" >&2
    usage
    exit 1
    ;;
esac
