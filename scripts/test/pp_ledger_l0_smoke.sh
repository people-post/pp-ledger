#!/usr/bin/env bash
# L-SMOKE-L0: binaries up; pp-client status reaches beacon (via relay) and a miner.
# Hard-fails on RPC / parse errors.
#
# Usage:
#   ./scripts/test/pp_ledger_l0_smoke.sh
#   PP_LEDGER_SMOKE_ASSUME_UP=1 ./scripts/test/pp_ledger_l0_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

ASSUME_UP="${PP_LEDGER_SMOKE_ASSUME_UP:-0}"
TIMEOUT_SEC="${PP_LEDGER_SMOKE_L0_TIMEOUT_SEC:-60}"

if [[ "$ASSUME_UP" != "1" ]]; then
  stop_network || true
  network_up
fi

verify_build
[[ -f "$PID_FILE" ]] || die "network not up (no PID file); run pp_ledger_network.sh up"

wait_for_beacon_rpc "$TIMEOUT_SEC"
state=$(fetch_beacon_state)
tip=$(parse_next_block_id "$state")
[[ -n "$tip" ]] || die "L-SMOKE-L0: beacon status missing nextBlockId"
echo -e "${GREEN}✓ Beacon status OK (nextBlockId=$tip)${NC}"

miner_id=$(wait_for_miner_ready "$TIMEOUT_SEC")
mstate=$(fetch_miner_status "$miner_id")
mtip=$(parse_next_block_id "$mstate")
[[ -n "$mtip" ]] || die "L-SMOKE-L0: miner status missing nextBlockId"
echo -e "${GREEN}✓ Miner${miner_id} status OK (nextBlockId=$mtip)${NC}"

echo -e "${GREEN}L-SMOKE-L0 PASSED${NC}"
