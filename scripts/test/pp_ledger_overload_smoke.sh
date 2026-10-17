#!/usr/bin/env bash
# L-LOAD-OVERLOAD (projects/load-resilience lr8, goal 1): floods on the relay — cheap and expensive
# reads, forged transfers and registrations, parked waits, large bodies, a herd of new clients,
# valid transfers past the pool — then on the beacon and on a miner, each for 8 s at thousands a
# second, with 10 transfers/s beside (through the relay) and the flooded node's HEALTH probed
# throughout. Each must leave the node alive and bounded, the chain's duties going on (blocks grow,
# no miner's block lost), the excess answered fast (no timeout), the transfers beside served.
#
# Usage:
#   ./scripts/test/pp_ledger_overload_smoke.sh
#   PP_LEDGER_OVERLOAD_ONLY="herd wait" ./scripts/test/pp_ledger_overload_smoke.sh   # some floods only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Renewals once in a long while, as on a real network (LR004).
export PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS:-1000}"
# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

readonly HEALTH_MAX_MS=2000      # HEALTH answers within this, every probe
readonly STEADY_P99_MS=2000      # the transfers beside: replies
readonly MAX_RSS=$((512 << 20))  # the flooded node's resident memory

stop_network || true
network_up
wait_for_beacon_rpc 60

relay="$(relay_multiaddr)"
beacon="$(beacon_multiaddr)"
client="$BUILD_DIR/app/pp-client"
accounts="${TEST_DIR}/load-accounts.json"
echo -e "${BLUE}═══ Funded accounts for the load ═══${NC}"
load_accounts "$accounts" 100
socket=$(health_field "$relay" udpReceiveBufferBytes)
echo -e "${CYAN}Relay UDP receive buffer: ${socket} B (8 MiB needs net.core.rmem_max or CAP_NET_ADMIN)${NC}"

node_multiaddr() {
  case $1 in
  beacon) beacon_multiaddr ;;
  miner*) miner_multiaddr "${1#miner}" ;;
  *) relay_multiaddr "$1" ;;
  esac
}

abandoned() {
  local total=0 n v
  for n in 1 2 3; do
    v=$(health_field "$(miner_multiaddr "$n")" blocksAbandoned)
    total=$((total + ${v:-0}))
  done
  echo "$total"
}

# flood <name> <node> <kind> <flood reply p99 max, ms> <pool: steady may meet a full pool (0/1)>
#       <unsent: per mille of the flood that may find no link (a herd of new clients past the handshake
#        backlog retries, as TCP's SYNs do, and a few may give up)> <load args...>
flood() {
  local name=$1 node=$2 kind=$3 p99=$4 pool=$5 unsent=$6
  shift 6
  local target
  target=$(node_multiaddr "$node")
  if [[ -n "${PP_LEDGER_OVERLOAD_ONLY:-}" && " ${PP_LEDGER_OVERLOAD_ONLY} " != *" ${name} "* ]]; then
    return 0
  fi
  echo -e "${BLUE}═══ L-LOAD-OVERLOAD ${name} on ${node}: $* ═══${NC}"
  echo -e "${CYAN}  ${node} links before: $(health_field "$target" links)${NC}"
  local steady="${TEST_DIR}/overload-${name}-steady.json" load="${TEST_DIR}/overload-${name}.json"
  local probes="${TEST_DIR}/overload-${name}-health.txt"
  local next0 lost0
  next0=$(health_field "$beacon" nextBlockId)
  lost0=$(abandoned)
  # Behind a full pool the transfers beside wait for its backlog (~blocks of 100): follow them longer.
  local drain=60 quiet=15
  ((pool)) && drain=300 quiet=120
  timeout 600 "$client" -b --host "$relay" load --accounts "$accounts" --mix tx_add -r 10 -d 20 --drain "$drain" --quiet "$quiet" \
    --json "$steady" >"${steady%.json}.log" 2>&1 &
  local steady_pid=$!
  sleep 4
  timeout 200 "$client" -b --host "$target" load --accounts "$accounts" "$@" --drain 0 --json "$load" \
    >"${load%.json}.log" 2>&1 &
  local load_pid=$!
  : >"$probes"
  local i start
  for i in $(seq 1 16); do
    start=$(date +%s%N)
    if timeout 10 "$client" -b --host "$target" health >/dev/null 2>&1; then
      echo "ok $((($(date +%s%N) - start) / 1000000))" >>"$probes"
    else
      echo "fail $((($(date +%s%N) - start) / 1000000))" >>"$probes"
    fi
    sleep 0.5
  done
  wait "$load_pid" || die "load (${name}) failed: $(tail -3 "${load%.json}.log")"
  wait "$steady_pid" || die "load beside (${name}) failed: $(tail -3 "${steady%.json}.log")"
  tail -4 "${load%.json}.log" | sed 's/^/    /'

  load_check "$load" "k('${kind}', 'timeout') == 0" "${name}: every request sent was answered (none timed out)"
  load_check "$load" "k('${kind}', 'unsent') * 1000 <= k('${kind}', 'sent') * ${unsent}" \
    "${name}: at most ${unsent}‰ found no link"
  load_check "$load" "k('${kind}', 'replyMsP99') <= ${p99}" "${name}: replies p99 within ${p99} ms"
  local worst fails
  worst=$(awk '{print $2}' "$probes" | sort -n | tail -1)
  fails=$(grep -c '^fail' "$probes" || true)
  ((fails == 0 && worst <= HEALTH_MAX_MS)) ||
    die "${name}: the ${node}'s HEALTH ${fails} unanswered, slowest ${worst} ms (at most ${HEALTH_MAX_MS})"
  echo -e "${GREEN}  ✓ ${name}: the ${node}'s HEALTH answered every time (slowest ${worst} ms)${NC}"
  if ((pool)); then
    load_check "$steady" "k('tx_add', 'taken') + k('tx_add', 'pool_full') + k('tx_add', 'busy') == k('tx_add', 'sent') and r['included'] == r['taken']" \
      "${name}: the transfers beside taken (or told the pool is full) and included"
  else
    load_check "$steady" "r['taken'] == r['validSent'] == r['included']" \
      "${name}: the transfers beside all taken and included"
  fi
  load_check "$steady" "k('tx_add', 'replyMsP99') <= ${STEADY_P99_MS}" \
    "${name}: the transfers beside answered within ${STEADY_P99_MS} ms (p99)"
  local next1 lost1 rss ready
  next1=$(health_field "$beacon" nextBlockId)
  lost1=$(abandoned)
  ((next1 >= next0 + 5)) || die "${name}: the chain stalled (nextBlockId ${next0} → ${next1})"
  ((lost1 == lost0)) || die "${name}: $((lost1 - lost0)) miner block(s) taken by no upstream"
  echo -e "${GREEN}  ✓ ${name}: the chain grew (${next0} → ${next1}), no miner's block lost${NC}"
  rss=$(health_field "$target" rssBytes)
  ready=$(health_field "$target" ready)
  [[ "$ready" == "True" ]] || die "${name}: the ${node} is not ready after the flood"
  ((${rss:-0} <= MAX_RSS)) || die "${name}: the ${node} holds ${rss} B (at most ${MAX_RSS})"
  echo -e "${GREEN}  ✓ ${name}: the ${node} ready after, $((${rss:-0} >> 20)) MiB resident${NC}"
}

#     name              node    kind               p99 ms pool unsent‰ load
flood status            relay   status             2000   0    0       --mix status -r 8000 -d 8 --peers 64
flood herd              relay   status             2000   0    20      --mix status -r 800 -d 8 --peers 400
flood wallet            relay   tx_get_by_wallet   2000   0    0       --mix tx_get_by_wallet -r 2000 -d 8 --peers 64
flood junk              relay   tx_junk            2000   0    0       --mix tx_junk -r 4000 -d 8 --peers 64
flood register          relay   register           2000   0    0       --mix register -r 6000 -d 8 --peers 64
flood wait              relay   block_wait         8000   0    20      --mix block_wait -r 2000 -d 8 --peers 400
flood big               relay   big                15000  0    0       --mix big -r 64 -d 8 --peers 64
flood beacon-status     beacon  status             2000   0    0       --mix status -r 8000 -d 8 --peers 64
flood beacon-register   beacon  register           2000   0    0       --mix register -r 6000 -d 8 --peers 64
flood beacon-big        beacon  big                15000  0    0       --mix big -r 64 -d 8 --peers 64
flood miner-status      miner1  status             2000   0    0       --mix status -r 8000 -d 8 --peers 64
flood miner-junk        miner1  tx_junk            2000   0    0       --mix tx_junk -r 4000 -d 8 --peers 64
flood miner-big         miner1  big                15000  0    0       --mix big -r 64 -d 8 --peers 64
# Last: they leave the pool full for a while. A miner forwards transfers up through the relay.
flood miner-transfers   miner1  tx_add             2000   1    0       --mix tx_add -r 2000 -d 8 --peers 64
flood transfers         relay   tx_add             2000   1    0       --mix tx_add -r 2000 -d 8 --peers 64

stop_network || true
echo -e "${GREEN}L-LOAD-OVERLOAD PASSED${NC}"
