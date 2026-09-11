#!/usr/bin/env bash
# Compatibility wrapper for checkpoint / late-join dogfood.
# Canonical LATEJOIN assert: scripts/test/pp_ledger_latejoin_smoke.sh
# Prefer: ./scripts/test/pp_ledger_local_test.sh run --suite latejoin
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/test/pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

usage() {
  cat <<EOF
Usage: $0 [run|start|stop|clear]

  run    Bring up smoke network, inject txs, run LATEJOIN + observation
  start  Start network only (alias of scripts/test/pp_ledger_network.sh up)
  stop   Stop network
  clear  Stop and remove smoke data

Hard assert lives in L-SMOKE-LATEJOIN (pp_ledger_latejoin_smoke.sh).
Scenarios 1/2/4 remain observation-only (non-gating).
EOF
}

observe_basic_cycles() {
  echo -e "${BLUE}═══ Observation: basic cycles (non-gating) ═══${NC}"
  wait_for_blocks 3 60 || true
  fetch_beacon_state 2>/dev/null | head -20 || true
}

observe_multi_cycle() {
  echo -e "${BLUE}═══ Observation: multi-slot (non-gating) ═══${NC}"
  local i
  for i in 1 2 3; do
    sleep $((SLOT_DURATION * 2))
    local state next
    state=$(fetch_beacon_state 2>/dev/null || true)
    next=$(parse_next_block_id "$state")
    echo "  Check $i: nextBlockId=${next:-?}"
  done
}

cmd=${1:-run}
case "$cmd" in
  stop)
    stop_network
    ;;
  clear|clean)
    clear_network
    ;;
  start)
    network_up
    ;;
  run|"")
    verify_build
    stop_network || true
    network_up
    inject_transactions_for_block_production
    observe_basic_cycles
    run_latejoin_scenario
    observe_multi_cycle
    echo -e "${GREEN}Checkpoint cycles dogfood complete (LATEJOIN hard-asserted)${NC}"
    echo "Network still running. Stop: $0 stop   Clear: $0 clear"
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
