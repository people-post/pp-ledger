#!/usr/bin/env bash
# L-SMOKE-L0: fail-fast layers L0a (PIDs) → L0b (beacon RPC) → L0c (miner RPC).
# Hard-fails on process death / RPC / parse errors; dumps artifacts under $TEST_DIR/artifacts.
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

assert_l0a_pids_alive
assert_l0b_beacon_rpc "$TIMEOUT_SEC"
assert_l0c_miner_rpc "$TIMEOUT_SEC"

echo -e "${GREEN}L-SMOKE-L0 PASSED${NC}"
