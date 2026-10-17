#!/usr/bin/env bash
# L-SMOKE-REGENESIS (beacon-authority bp10, case 1: re-genesis): an old network runs, then ends; its
# state is exported at its last block; a new network imports it (pp-beacon --init --import-state);
# the relay and miners join it fresh, each checking it against its own old ledger, and it goes on
# with the old miners' keys. Runbook: docs/ops/RE_GENESIS.md.
#
# Usage:
#   ./scripts/test/pp_ledger_regenesis_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

stop_network || true
verify_build
rm -rf "${TEST_DIR}/regenesis" "${TEST_DIR}/state.snap"
mkdir -p "$TEST_DIR" "$MULTIADDR_DIR"
initialize_beacon_with_test_config
create_beacon_config
start_beacon
start_relay
start_all_miners
inject_transactions_for_block_production

wait_for_beacon_rpc 60
run_regenesis_scenario
stop_network || true
echo -e "${GREEN}L-SMOKE-REGENESIS PASSED${NC}"
