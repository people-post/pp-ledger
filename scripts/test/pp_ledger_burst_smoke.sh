#!/usr/bin/env bash
# L-LOAD-JUNK, L-LOAD-BURST (projects/load-resilience lr7, goal 2): beacon, relay → miners, loaded
# through the relay with pp-client load. Junk never takes a place in the pool; a burst five blocks
# deep, from 64 new clients at once on top of a steady stream, is taken whole and included within
# about the blocks it fills, every client told the truth.
#
# Usage:
#   ./scripts/test/pp_ledger_burst_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Renewals once in a long while, as on a real network (LR004): the fixture's renewal every two
# blocks would turn the accounts created here into a renewal storm that holds every transaction.
export PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS:-1000}"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

stop_network || true
network_up
wait_for_beacon_rpc 60

relay="$(relay_multiaddr)"
accounts="${TEST_DIR}/load-accounts.json"
block_capacity=100 # maxTransactionsPerBlock in the fixture
echo -e "${BLUE}═══ Funded accounts for the load ═══${NC}"
load_accounts "$accounts" 200

echo -e "${BLUE}═══ L-LOAD-JUNK: 20/s for 15 s, one in ten badly signed ═══${NC}"
junk="${TEST_DIR}/load-junk.json"
timeout 600 "$BUILD_DIR/app/pp-client" -b --host "$relay" load --accounts "$accounts" \
  --mix tx_add=9,tx_junk=1 -r 20 -d 15 --json "$junk" || die "load (junk) failed"
load_check "$junk" 'k("tx_junk", "taken") == 0 and k("tx_junk", "refused") == k("tx_junk", "sent")' \
  "every junk transfer refused before the pool"
load_check "$junk" 'r["junkIncluded"] == 0' "no junk in any block"
load_check "$junk" 'r["taken"] == r["validSent"] == r["included"]' "every valid transfer taken and included"
load_check "$junk" 'r["inclusionBlocksMax"] <= 3' "each within three blocks"

echo -e "${BLUE}═══ L-LOAD-BURST: 500 at once from 64 new clients, on 10/s for 20 s ═══${NC}"
burst="${TEST_DIR}/load-burst.json"
timeout 900 "$BUILD_DIR/app/pp-client" -b --host "$relay" load --accounts "$accounts" \
  -r 10 -d 20 --burst 500 --burst-at 2 --peers 64 --drain 120 --json "$burst" || die "load (burst) failed"
load_check "$burst" 'r["taken"] == r["validSent"]' "every transfer of the burst taken (none bounced)"
load_check "$burst" 'r["included"] == r["validSent"]' "every transfer included"
load_check "$burst" 'r["includedNotTaken"] == 0' "no client told a failure for a transfer that went through"
blocks=$(( (700 + block_capacity - 1) / block_capacity + 3 ))
load_check "$burst" "r['inclusionBlocksMax'] <= ${blocks}" "the burst drained within ${blocks} blocks"

stop_network || true
echo -e "${GREEN}L-LOAD-BURST PASSED${NC}"
