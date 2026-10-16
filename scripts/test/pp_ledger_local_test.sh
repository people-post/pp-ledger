#!/usr/bin/env bash
# Local test driver for pp-ledger: unit ctest + multi-process smokes.
#
# Owns smoke network up/stop/clear. Individual asserts live in pp_ledger_*_smoke.sh.
#
# Suites: unit | l0 | l1 | latejoin | smoke | rotation | takeover | move | image
#   smoke == l0 → l1 → latejoin on one network (brought up once).
#   rotation brings up its own network (the next beacon key is staged before the beacon starts).
#   takeover brings up its own network (case 2: a relay promoted to be the new beacon).
#   move brings up its own network (case 3: the beacon moved to another host — here, port).
# See docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${PP_LEDGER_BUILD_DIR:-${ROOT}/build}"
export PP_LEDGER_BUILD_DIR="$BUILD_DIR"

# shellcheck source=pp_ledger_smoke_lib.sh
source "${ROOT}/scripts/test/pp_ledger_smoke_lib.sh"

SUITE="smoke"
DOWN_AFTER=0

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [options]

Commands:
  run       Run --suite (default: smoke). Leaves network up unless --down.
  up        Start smoke network
  stop      Stop smoke network (data kept)
  clear     Stop and remove smoke data dir
  status    Process + best-effort RPC status

Options (run):
  --suite unit|l0|l1|latejoin|smoke|rotation|takeover|move|image
  --down                       stop network after run

Examples:
  $(basename "$0") run --suite unit
  $(basename "$0") run --suite l0
  $(basename "$0") run --suite smoke --down   # l0 + l1 + latejoin, one network
  $(basename "$0") run --suite image
EOF
}

die_usage() {
  echo "error: $*" >&2
  usage >&2
  exit 2
}

run_unit() {
  verify_build
  if [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
    die "ctest not configured; rebuild with -DPP_LEDGER_BUILD_TESTS=ON"
  fi
  echo -e "${BLUE}═══ suite unit (ctest) ═══${NC}"
  (cd "$BUILD_DIR" && ctest --output-on-failure -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)")
}

ensure_network() {
  if pids_tree_healthy; then
    echo -e "${CYAN}Reusing healthy smoke network${NC}"
    return 0
  fi
  if [[ -f "$PID_FILE" ]]; then
    echo -e "${YELLOW}Stale/dead PID tree; restarting smoke network${NC}"
    stop_network || true
  fi
  network_up
}

run_l0() {
  ensure_network
  PP_LEDGER_SMOKE_ASSUME_UP=1 bash "${ROOT}/scripts/test/pp_ledger_l0_smoke.sh"
}

run_l1() {
  ensure_network
  PP_LEDGER_SMOKE_ASSUME_UP=1 bash "${ROOT}/scripts/test/pp_ledger_l1_smoke.sh"
}

run_latejoin() {
  ensure_network
  # Latejoin needs tip progress context; inject once if tip is still genesis-ish.
  wait_for_beacon_rpc 60
  inject_transactions_for_block_production || true
  PP_LEDGER_SMOKE_ASSUME_UP=1 bash "${ROOT}/scripts/test/pp_ledger_latejoin_smoke.sh"
}

run_smoke() {
  # Every suite on one network: bring-up is most of the cost, the suites
  # themselves take seconds (latejoin restarts miner3 within the network).
  echo -e "${CYAN}suite smoke → l0, l1, latejoin on one network${NC}"
  run_l0
  run_l1
  run_latejoin
}

run_takeover() {
  # Its own network: relay2 becomes the beacon.
  bash "${ROOT}/scripts/test/pp_ledger_takeover_smoke.sh"
}

run_move() {
  # Its own network: the beacon moves to another port (case 3).
  bash "${ROOT}/scripts/test/pp_ledger_move_smoke.sh"
}

run_rotation() {
  # Its own network: the beacon loads its next signing key at start.
  bash "${ROOT}/scripts/test/pp_ledger_rotation_smoke.sh"
}

run_image() {
  # The image and the sample stack (deploy/compose.yml) as an operator runs them: build the image,
  # deploy/init.sh (keys, genesis, peer ids from the environment), `docker compose up --wait`, then
  # readiness, chain growth, the HTTP API and a clean stop. Own project, subnet and host ports.
  echo -e "${BLUE}═══ suite image (L-SMOKE-IMAGE: compose stack from the environment) ═══${NC}"
  command -v docker >/dev/null 2>&1 || die "docker required for --suite image"
  local image="${PP_LEDGER_IMAGE:-}"
  if [[ -z "$image" ]]; then
    image="pp-ledger:smoke"
    echo -e "${CYAN}Building ${image} (set PP_LEDGER_IMAGE to use an existing one)...${NC}"
    docker build -t "$image" "${ROOT}" >"${ROOT}/build/docker-build.log" 2>&1 ||
      die "docker build failed: see build/docker-build.log"
  fi
  local stack
  stack=$(mktemp -d)
  cp "${ROOT}/deploy/compose.yml" "${ROOT}/deploy/init.sh" "$stack/"
  (
    cd "$stack"
    export COMPOSE_PROJECT_NAME=pp-ledger-image-smoke PP_LEDGER_IMAGE="$image" \
      PP_LEDGER_SLOT_DURATION=2 PP_LEDGER_SLOTS_PER_EPOCH=10 PP_LEDGER_NET="${PP_LEDGER_SMOKE_IMAGE_NET:-172.29.7}" \
      PP_LEDGER_BEACON_HOST_PORT=28517 PP_LEDGER_RELAY_HOST_PORT=28519 PP_LEDGER_MINER_HOST_PORT=28518 \
      PP_LEDGER_HTTP_HOST_PORT=28080
    cleanup() { docker compose down -v >/dev/null 2>&1 || true; }
    trap cleanup EXIT
    ./init.sh >init.log 2>&1 || { cat init.log; die "L-SMOKE-IMAGE: init.sh failed"; }
    docker compose up -d --wait --wait-timeout 240 >up.log 2>&1 || {
      cat up.log; docker compose logs --tail 40; die "L-SMOKE-IMAGE: the stack did not become healthy"; }
    echo -e "${GREEN}✓ beacon, relay, miner, http healthy${NC}"
    # Readiness over each node's RPC, as the image's HEALTHCHECK probes liveness (no HTTP in nodes).
    local s out deadline
    for s in beacon relay miner; do
      deadline=$((SECONDS + 60))
      until out=$(docker compose exec -T "$s" pp-client --host-file /data/listen.multiaddr health --ready 2>&1); do
        ((SECONDS < deadline)) || die "L-SMOKE-IMAGE: ${s} not ready: ${out}"
        sleep 2
      done
    done
    # Metrics come from pp-http, which asks the nodes over the RPC.
    local metrics
    metrics=$(curl -fsS localhost:28080/metrics) || die "L-SMOKE-IMAGE: pp-http /metrics"
    for s in beacon miner core-beacon; do
      grep -q "^pp_ledger_ready{node=\"${s}\"} 1$" <<<"$metrics" || die "L-SMOKE-IMAGE: /metrics: ${s} not ready"
    done
    next() { curl -s localhost:28080/metrics | sed -n 's/^pp_ledger_next_block_id{node="beacon"} //p'; }
    local before after
    deadline=$((SECONDS + 90))
    before=$(next)
    after=$before
    while ((SECONDS < deadline)) && [[ "$after" -le "$before" ]]; do
      sleep 3
      after=$(next)
    done
    [[ "$after" -gt "$before" ]] || die "L-SMOKE-IMAGE: the chain did not grow (nextBlockId ${before})"
    curl -fsS localhost:28080/api/beacon/state >/dev/null || die "L-SMOKE-IMAGE: HTTP API"
    echo -e "${GREEN}✓ ready (pp-client health, pp-http /metrics); chain grows (nextBlockId ${before} → ${after}); HTTP API answers${NC}"
    docker compose stop -t 20 >/dev/null 2>&1
    local bad
    bad=$(docker compose ps -a --format '{{.Service}} {{.ExitCode}}' | awk '$2 != 0' || true)
    [[ -z "$bad" ]] || die "L-SMOKE-IMAGE: unclean stop: ${bad}"
    echo -e "${GREEN}✓ every container stopped cleanly (exit 0)${NC}"
  )
  local status=$?
  rm -rf "$stack"
  [[ $status -eq 0 ]] || exit $status
  echo -e "${GREEN}L-SMOKE-IMAGE PASSED${NC}"
}

CMD=${1:-}
if [[ -z "$CMD" ]]; then
  die_usage "missing command"
fi
shift || true

case "$CMD" in
  run)
    while [[ $# -gt 0 ]]; do
      case "$1" in
        --suite)
          SUITE="${2:-}"
          [[ -n "$SUITE" ]] || die_usage "--suite needs a value"
          shift 2
          ;;
        --down)
          DOWN_AFTER=1
          shift
          ;;
        -h|--help)
          usage
          exit 0
          ;;
        *)
          die_usage "unknown option: $1"
          ;;
      esac
    done
    case "$SUITE" in
      unit) run_unit ;;
      l0) run_l0 ;;
      l1) run_l1 ;;
      latejoin) run_latejoin ;;
      smoke) run_smoke ;;
      rotation) run_rotation ;;
      takeover) run_takeover ;;
      move) run_move ;;
      image) run_image ;;
      *) die_usage "unknown suite: $SUITE" ;;
    esac
    if [[ "$DOWN_AFTER" == "1" && "$SUITE" != "unit" && "$SUITE" != "image" ]]; then
      stop_network
    fi
    ;;
  up) network_up ;;
  stop) stop_network ;;
  clear|clean) clear_network ;;
  status) network_status ;;
  -h|--help|help) usage ;;
  *) die_usage "unknown command: $CMD" ;;
esac
