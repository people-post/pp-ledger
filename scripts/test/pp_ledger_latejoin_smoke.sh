#!/usr/bin/env bash
# L-SMOKE-LATEJOIN: stop miner3, let tip move, restart, assert miner tip catches beacon.
#
# Usage:
#   ./scripts/test/pp_ledger_latejoin_smoke.sh
#   PP_LEDGER_SMOKE_ASSUME_UP=1 ./scripts/test/pp_ledger_latejoin_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

ASSUME_UP="${PP_LEDGER_SMOKE_ASSUME_UP:-0}"

if [[ "$ASSUME_UP" != "1" ]]; then
  stop_network || true
  network_up
  inject_transactions_for_block_production
fi

verify_build
wait_for_beacon_rpc 60
run_latejoin_scenario
echo -e "${GREEN}L-SMOKE-LATEJOIN PASSED${NC}"
