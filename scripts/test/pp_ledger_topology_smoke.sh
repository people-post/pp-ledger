#!/usr/bin/env bash
# L-LOAD-TOPOLOGY (projects/load-resilience lr10): what only shows between nodes.
#   start-up    transfers from the first slots: no block refused late, none lost (M13)
#   fan-out     relays added behind the beacon (+4, +8), transfers through the first: the beacon keeps
#               up, every relay stays within the readiness lag, slots keep their blocks
#   load        transfers near the designed rate with a read flood on the relay: slots keep their blocks
#   churn       the miners' relay restarted, then each miner in turn: the chain resumes or goes on,
#               transfers are taken and included, every miner is registered again
#
# Usage:
#   ./scripts/test/pp_ledger_topology_smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS:-1000}"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

readonly EXTRA_RELAY_BASE_PORT="${PP_LEDGER_TOPOLOGY_RELAY_BASE_PORT:-8640}"
readonly MIN_SLOT_HIT=0.8 # blocks per slot while transfers wait: ~0.9 measured (a run edge), a lost miner ~0.67

client="$BUILD_DIR/app/pp-client"
accounts="${TEST_DIR}/load-accounts.json"

# block_slot <id>: the slot of block <id> (from the beacon).
block_slot() {
  timeout 10 "$client" -b --host "$(beacon_multiaddr)" block "$1" 2>/dev/null |
    python3 -c 'import json, sys; print(json.load(sys.stdin)["block"]["slot"])' 2>/dev/null || echo ""
}

# status_field <multiaddr> <field>: a field of the node's STATUS (JSON after a header line).
status_field() {
  timeout 10 "$client" -b --host "$1" status 2>/dev/null | python3 -c '
import json, sys
text = sys.stdin.read()
try:
    print(json.loads(text[text.index("{"):], strict=False).get(sys.argv[1], ""))
except Exception:
    print("")' "$2"
}

late_blocks() {
  cat "${TEST_DIR}"/miner*/console.log 2>/dev/null | grep -a -c "delivery deadline" || true
}

abandoned() {
  local total=0 n v
  for n in 1 2 3; do
    v=$(health_field "$(miner_multiaddr "$n")" blocksAbandoned)
    total=$((total + ${v:-0}))
  done
  echo "$total"
}

# steady <name> <rate> <seconds> [flood load args...]: transfers through the relay (and a flood beside
# it), then: every transfer taken and included within 3 blocks, slots kept their blocks, no block lost.
steady() {
  local name=$1 rate=$2 seconds=$3
  shift 3
  local report="${TEST_DIR}/topology-${name}.json" lost0
  lost0=$(abandoned)
  local flood_pid=""
  if (($# > 0)); then
    timeout 300 "$client" -b --host "$(relay_multiaddr)" load "$@" --drain 0 >"${report%.json}-flood.log" 2>&1 &
    flood_pid=$!
  fi
  timeout 600 "$client" -b --host "$(relay_multiaddr)" load --accounts "$accounts" --mix tx_add -r "$rate" \
    -d "$seconds" --drain 60 --json "$report" >"${report%.json}.log" 2>&1 || die "load (${name}) failed"
  [[ -z "$flood_pid" ]] || wait "$flood_pid" || true
  local lost1
  lost1=$(abandoned)
  load_check "$report" "r['taken'] == r['validSent'] == r['included'] and r['inclusionBlocksMax'] <= 3" \
    "${name}: every transfer taken and included within 3 blocks"
  # From the first block that took a transfer to the last, transfers waited in every slot: each slot
  # should have its block.
  local first last
  first=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["firstBlock"])' "$report")
  last=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["lastBlock"])' "$report")
  python3 - "$first" "$last" "$(block_slot "$first")" "$(block_slot "$last")" "$MIN_SLOT_HIT" "$name" <<'PY' ||
import sys
b0, b1, s0, s1, floor, name = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5]), sys.argv[6]
hit = (b1 - b0 + 1) / max(1, s1 - s0 + 1)
print(f"  {name}: blocks {b0}-{b1} in slots {s0}-{s1}: {hit:.2f} of the slots had their block")
sys.exit(0 if hit >= floor else 1)
PY
    die "${name}: slots went without their block"
  ((lost1 == lost0)) || die "${name}: $((lost1 - lost0)) miner block(s) taken by no upstream"
  echo -e "${GREEN}  ✓ ${name}: slots kept their blocks, no miner's block lost${NC}"
}

# relays_in_step <names...>: each relay within 3 blocks of the beacon.
relays_in_step() {
  local tip name next
  tip=$(health_field "$(beacon_multiaddr)" nextBlockId)
  for name in "$@"; do
    next=$(health_field "$(relay_multiaddr "$name")" nextBlockId)
    ((${next:-0} + 3 >= tip)) || die "${name} is behind: block ${next:-?} of ${tip}"
  done
  echo -e "${GREEN}  ✓ every relay within 3 blocks of the beacon (${tip})${NC}"
}

stop_network || true
network_up
wait_for_beacon_rpc 60

echo -e "${BLUE}═══ L-LOAD-TOPOLOGY start-up: transfers from the first slots ═══${NC}"
load_accounts "$accounts" 100
late=$(late_blocks)
lost=$(abandoned)
((late == 0 && lost == 0)) || die "start-up: ${late} block(s) refused late, ${lost} lost (M13)"
echo -e "${GREEN}  ✓ start-up: 100 accounts created, no block refused late, none lost${NC}"

echo -e "${BLUE}═══ L-LOAD-TOPOLOGY fan-out: relays behind the beacon ═══${NC}"
steady fanout-1 20 20
extra=()
for step in 4 8; do
  while ((${#extra[@]} < step)); do
    name="relayx${#extra[@]}"
    start_relay "$name" $((EXTRA_RELAY_BASE_PORT + ${#extra[@]}))
    extra+=("$name")
  done
  echo -e "${CYAN}  $((step + 1)) relays follow the beacon; its health: loop turn max $(health_field "$(beacon_multiaddr)" loopTurnMaxMs) ms, $(health_field "$(beacon_multiaddr)" links) links${NC}"
  steady "fanout-$((step + 1))" 20 20
  relays_in_step relay "${extra[@]}"
done
turn=$(health_field "$(beacon_multiaddr)" loopTurnMaxMs)
((${turn:-0} < 1000)) || die "the beacon's run loop took ${turn} ms with 9 relays"
echo -e "${GREEN}  ✓ fan-out: the beacon's longest run-loop turn ${turn} ms with 9 relays${NC}"

echo -e "${BLUE}═══ L-LOAD-TOPOLOGY load: near the designed rate, a read flood on the relay ═══${NC}"
steady load 40 20 --mix status -r 4000 -d 20 --peers 64

echo -e "${BLUE}═══ L-LOAD-TOPOLOGY churn: the miners' relay restarted ═══${NC}"
next0=$(status_field "$(beacon_multiaddr)" nextBlockId)
stop_node relay
sleep 10
start_relay relay "$RELAY_PORT"
for i in $(seq 1 60); do
  peers=$(health_field "$(relay_multiaddr)" minerPeers)
  ((${peers:-0} >= 3)) && break
  sleep 1
done
((${peers:-0} >= 3)) || die "churn: only ${peers:-0} miner(s) registered again through the restarted relay"
echo -e "${GREEN}  ✓ churn: every miner registered again through the restarted relay (${i} s)${NC}"
steady relay-restarted 20 10

echo -e "${BLUE}═══ L-LOAD-TOPOLOGY churn: each miner restarted in turn ═══${NC}"
timeout 600 "$client" -b --host "$(relay_multiaddr)" load --accounts "$accounts" --mix tx_add -r 10 -d 60 \
  --drain 60 --json "${TEST_DIR}/topology-rolling.json" >"${TEST_DIR}/topology-rolling.log" 2>&1 &
rolling=$!
for n in 1 2 3; do
  before=$(status_field "$(beacon_multiaddr)" nextBlockId)
  stop_node "miner${n}"
  sleep 8
  after=$(status_field "$(beacon_multiaddr)" nextBlockId)
  ((after > before)) || die "churn: the chain stopped while miner${n} was down (${before} → ${after})"
  start_miner "$n" || die "miner${n} did not come back"
  echo -e "${GREEN}  ✓ miner${n} down 8 s: the chain went on (${before} → ${after}); back${NC}"
done
wait "$rolling" || die "load (rolling restart) failed"
load_check "${TEST_DIR}/topology-rolling.json" "r['taken'] == r['validSent'] == r['included']" \
  "churn: every transfer of the rolling restart taken and included"

stop_network || true
echo -e "${GREEN}L-LOAD-TOPOLOGY PASSED${NC}"
