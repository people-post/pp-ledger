#!/usr/bin/env bash
# L-SMOKE-TAKEOVER (beacon-authority bp3, case 2 end to end): beacon, relay1 → miners, and relay2 →
# relay3 beside them. The beacon key is stolen: the admins certify generation 2 for a key generated
# on relay2 with a cutover two blocks back and relay3 as the dial hint, and hand it to relay1 and
# relay2. Every node rewinds, the old beacon stands down, relay2 is promoted (pp-beacon --promote),
# a leader commits the certificate in the block after the cutover, and relay1 and the miners follow
# generation 2 through relay3.
#
# pp-http's /api/chain/ (a service's feed) reads relay1 throughout: an account's balance and its
# transfers before, generation 2 linked across the cutover after.
#
# Usage:
#   ./scripts/test/pp_ledger_takeover_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

stop_network || true
verify_build
mkdir -p "$TEST_DIR" "$MULTIADDR_DIR"
initialize_beacon_with_test_config
create_beacon_config
start_beacon
start_relay
start_relay relay2 "$RELAY2_PORT"
start_relay relay3 "$RELAY3_PORT" "$(relay_multiaddr relay2)"
start_all_miners
start_chain_api
inject_transactions_for_block_production

wait_for_beacon_rpc 60
check_chain_feed
run_takeover_scenario
check_chain_after_takeover "$TAKEOVER_CUTOVER"
stop_network || true
echo -e "${GREEN}L-SMOKE-TAKEOVER PASSED${NC}"
