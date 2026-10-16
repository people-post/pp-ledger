#!/usr/bin/env bash
# L-SMOKE-MOVE (beacon-authority bp4, case 3: move the server): beacon, relay → miners. The relay
# lists the beacon's new address beside its current one. The beacon stops cleanly, its whole work
# directory is copied, the old host is retired, and the copy starts on another port; the relay
# moves to it and the chain goes on under the same generation. Runbook:
# docs/ops/MOVING_THE_BEACON.md.
#
# Usage:
#   ./scripts/test/pp_ledger_move_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

stop_network || true
verify_build
mkdir -p "$TEST_DIR" "$MULTIADDR_DIR"
rm -rf "${TEST_DIR}/beacon-moved" "${TEST_DIR}/beacon-torn"
initialize_beacon_with_test_config
create_beacon_config
start_beacon
start_relay relay "$RELAY_PORT" "" "$(moved_beacon_multiaddr)"
start_all_miners
inject_transactions_for_block_production

wait_for_beacon_rpc 60
run_move_scenario
stop_network || true
echo -e "${GREEN}L-SMOKE-MOVE PASSED${NC}"
