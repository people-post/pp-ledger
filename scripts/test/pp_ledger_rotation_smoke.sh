#!/usr/bin/env bash
# L-SMOKE-EQUIVOCATION (beacon-authority ba6) first: evidence that the generation-1 key signed two
# blocks at one height, handed to the relay, halts the relay, the beacon and every miner; the
# rotation below (a later generation) resumes them, and the evidence is dropped.
#
# L-SMOKE-ROTATION (beacon-authority ba5, ba5b): stage a next beacon key, certify it as
# generation 2 (mk-beacon-cert → sign-tx ×3 → submit-tx), assert the beacon signs with it and
# miners follow. Then, once a checkpoint lies above the certificate, fresh miners start at it —
# one pinning genesis (tied to it by the authority proof), one not — warm up and produce. The relay
# pins genesis. Last (ba5c), the admins hand the relay a generation-3 certificate with a cutover
# below the tip, as for a stolen key: the relay serves it and no tip past it, the beacon (holding
# no key it names) stands down, and nothing commits.
#
# Usage:
#   ./scripts/test/pp_ledger_rotation_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Short epochs: checkpoints are epoch starts, so they come every few blocks and fresh miners start
# above genesis.
export PP_LEDGER_SMOKE_SLOTS_PER_EPOCH="${PP_LEDGER_SMOKE_SLOTS_PER_EPOCH:-3}"
export PP_LEDGER_SMOKE_RELAY_PIN_GENESIS=1
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

stop_network || true
verify_build
mkdir -p "$TEST_DIR" "$MULTIADDR_DIR"
initialize_beacon_with_test_config
create_beacon_config
stage_next_beacon_key
start_beacon
start_relay
start_all_miners
start_http
inject_transactions_for_block_production

wait_for_beacon_rpc 60
run_equivocation_halt_scenario
run_rotation_scenario
check_equivocation_resumed
run_out_of_band_scenario
stop_network || true
echo -e "${GREEN}L-SMOKE-ROTATION PASSED${NC}"
