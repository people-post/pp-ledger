#!/usr/bin/env bash
# L-SMOKE-L1: beacon→relay→miner tip advances after tx inject.
# Uses short slots; may be cost/flake without forced leader (see TEST_STRATEGY.md).
#
# Usage:
#   ./scripts/test/pp_ledger_l1_smoke.sh
#   PP_LEDGER_SMOKE_ASSUME_UP=1 ./scripts/test/pp_ledger_l1_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

ASSUME_UP="${PP_LEDGER_SMOKE_ASSUME_UP:-0}"
TIP_TIMEOUT="${PP_LEDGER_SMOKE_L1_TIMEOUT_SEC:-180}"

if [[ "$ASSUME_UP" != "1" ]]; then
  stop_network || true
  network_up
fi

verify_build
wait_for_beacon_rpc 60
start_tip=$(get_next_block_id)
echo -e "${CYAN}L-SMOKE-L1 start tip nextBlockId=$start_tip${NC}"

inject_transactions_for_block_production

if ! wait_for_tip_increase "$start_tip" "$TIP_TIMEOUT"; then
  die "L-SMOKE-L1: tip did not advance past $start_tip within ${TIP_TIMEOUT}s"
fi

end_tip=$(get_next_block_id)
if [[ "$end_tip" -le "$start_tip" ]]; then
  die "L-SMOKE-L1: tip did not increase (start=$start_tip end=$end_tip)"
fi

echo -e "${GREEN}L-SMOKE-L1 PASSED (nextBlockId $start_tip → $end_tip)${NC}"
