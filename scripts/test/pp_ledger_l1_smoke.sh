#!/usr/bin/env bash
# L-SMOKE-L1: beacon→relay→miner tip advances after tx inject; a miner registers; an operation
# file (signed apart by the reserve holders, merged) creates an account.
# Uses short slots + tx inject (see docs/ops/TEST_STRATEGY.md).
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

# Miner registration: a funded user account (alice, from the inject) opts in.
register_miner_account $((1 << 30)) "${TEST_DIR}/keys/alice.key" 120 ||
  die "L-SMOKE-L1: miner registration did not land"

# CONFIG_GET: the relay serves the config in force (what tools build transactions from).
config_reply=$(run_pp_client -b --host "$(relay_multiaddr)" config 2>&1) ||
  die "L-SMOKE-L1: pp-client config failed: ${config_reply}"
grep -q "Admin keys: 3 of 3" <<<"$config_reply" || die "L-SMOKE-L1: config shows no admin keys: ${config_reply}"
echo -e "${GREEN}✓ The relay serves the config in force${NC}"

# Operation files: a multi-signature transaction signed apart, merged and submitted.
run_operation_file_scenario 120

echo -e "${GREEN}L-SMOKE-L1 PASSED (nextBlockId $start_tip → $end_tip; miner registered; operation file)${NC}"
