#!/usr/bin/env bash
# L-LOAD-UPSTREAM (projects/load-resilience lr8, goal 1): a relay whose upstream is slow or gone.
# Slow: the beacon frozen (SIGSTOP) past the relay's forward budget. Absent: the beacon stopped. In
# both, transfers through the relay are answered before their client's deadline — "the upstream did
# not answer in time" or "busy", never a client left to time out — the relay's own reads and HEALTH
# go on, and once the beacon is back the relay syncs and transfers are taken and included again.
#
# Usage:
#   ./scripts/test/pp_ledger_upstream_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS:-1000}"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

readonly CLIENT_TIMEOUT_MS=6000 # each request's deadline; the relay forwards with 90 % of it
readonly FREEZE_SECONDS=10      # longer than the forward budget

stop_network || true
network_up
wait_for_beacon_rpc 60

relay="$(relay_multiaddr)"
client="$BUILD_DIR/app/pp-client"
accounts="${TEST_DIR}/load-accounts.json"
echo -e "${BLUE}═══ Funded accounts for the load ═══${NC}"
load_accounts "$accounts" 40

# beside <name>: transfers and reads through the relay for 6 s, HEALTH probed; then the checks that
# the relay answered for its upstream.
beside() {
  local name=$1
  local transfers="${TEST_DIR}/upstream-${name}.json" reads="${TEST_DIR}/upstream-${name}-reads.json"
  timeout 120 "$client" -b --host "$relay" load --accounts "$accounts" --mix tx_add -r 20 -d 6 --peers 8 \
    --timeout "$CLIENT_TIMEOUT_MS" --drain 0 --json "$transfers" >"${transfers%.json}.log" 2>&1 &
  local t=$!
  timeout 120 "$client" -b --host "$relay" load --mix status -r 50 -d 6 --peers 8 \
    --timeout "$CLIENT_TIMEOUT_MS" --drain 0 --json "$reads" >"${reads%.json}.log" 2>&1 &
  local r=$!
  local i worst=0 start ms
  for i in $(seq 1 12); do
    start=$(date +%s%N)
    timeout 10 "$client" -b --host "$relay" health >/dev/null 2>&1 || die "${name}: the relay's HEALTH unanswered"
    ms=$((($(date +%s%N) - start) / 1000000))
    ((ms > worst)) && worst=$ms
    sleep 0.5
  done
  wait "$t" || die "load (${name}) failed: $(tail -3 "${transfers%.json}.log")"
  wait "$r" || die "reads (${name}) failed: $(tail -3 "${reads%.json}.log")"
  sed 's/^/    /' "${transfers%.json}.log" | grep -E 'tx_add|error' | head -4
  load_check "$transfers" "k('tx_add', 'replyMsMax') < ${CLIENT_TIMEOUT_MS} and k('tx_add', 'unsent') == 0" \
    "${name}: every transfer answered by the relay before its client's deadline"
  load_check "$transfers" "k('tx_add', 'taken') == 0" "${name}: none taken while the beacon cannot take it"
  load_check "$reads" "k('status', 'served') == k('status', 'sent') and k('status', 'replyMsP99') <= 2000" \
    "${name}: the relay's own reads served"
  ((worst <= 2000)) || die "${name}: the relay's HEALTH took ${worst} ms"
  echo -e "${GREEN}  ✓ ${name}: the relay's HEALTH answered every time (slowest ${worst} ms)${NC}"
}

# back <name>: the relay syncs from the beacon again (an idle chain grows by a heartbeat block every
# few slots only), then transfers are taken and included: the chain grows with them.
back() {
  local name=$1 age="" i start=$SECONDS
  for i in $(seq 1 60); do
    age=$(health_field "$relay" lastSyncAgeSeconds)
    [[ -n "$age" ]] && ((age <= 2)) && break
    sleep 1
  done
  [[ -n "$age" ]] && ((age <= 2)) || die "${name}: the relay did not sync from the beacon again (last ${age:-never})"
  local synced=$((SECONDS - start)) next0 next1
  next0=$(health_field "$relay" nextBlockId)
  local after="${TEST_DIR}/upstream-${name}-after.json"
  timeout 300 "$client" -b --host "$relay" load --accounts "$accounts" --mix tx_add -r 10 -d 5 --peers 4 \
    --drain 60 --json "$after" >"${after%.json}.log" 2>&1 || die "load (${name}, after) failed"
  next1=$(health_field "$relay" nextBlockId)
  load_check "$after" "r['taken'] == r['validSent'] == r['included']" \
    "${name}: back — the relay synced within ${synced} s, transfers taken and included (blocks ${next0} → ${next1})"
}

echo -e "${BLUE}═══ L-LOAD-UPSTREAM slow: the beacon frozen ${FREEZE_SECONDS} s ═══${NC}"
beacon_pid=$(pid_for_name beacon)
[[ -n "$beacon_pid" ]] || die "no beacon pid"
kill -STOP "$beacon_pid"
trap 'kill -CONT "$beacon_pid" 2>/dev/null || true' EXIT
frozen_at=$SECONDS
beside slow
while ((SECONDS - frozen_at < FREEZE_SECONDS)); do sleep 1; done
kill -CONT "$beacon_pid"
back slow

echo -e "${BLUE}═══ L-LOAD-UPSTREAM absent: the beacon stopped ═══${NC}"
stop_node beacon
beside absent
start_beacon
back absent

stop_network || true
echo -e "${GREEN}L-LOAD-UPSTREAM PASSED${NC}"
