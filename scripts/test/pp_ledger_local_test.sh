#!/usr/bin/env bash
# Local test driver for pp-ledger: unit ctest + multi-process smokes.
#
# Owns smoke network up/stop/clear. Individual asserts live in pp_ledger_*_smoke.sh.
#
# Suites: unit | l0 | l1 | latejoin | smoke | image
#   smoke == l0 only until Amp OsUdp dial is green (l1/latejoin stay explicit).
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
  --suite unit|l0|l1|latejoin|smoke|image
  --down                       stop network after run

Examples:
  $(basename "$0") run --suite unit
  $(basename "$0") run --suite l0
  $(basename "$0") run --suite smoke --down
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
  (cd "$BUILD_DIR" && ctest --output-on-failure)
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
  # Until Amp OsUdp localhost dial is green, default smoke is L0 only.
  # Run --suite l1 / latejoin explicitly for deeper checks.
  echo -e "${CYAN}suite smoke → l0 (l1/latejoin are separate until Amp dial is green)${NC}"
  run_l0
}

run_image() {
  echo -e "${BLUE}═══ suite image (compose smoke scaffold) ═══${NC}"
  local compose="${ROOT}/deploy/compose.smoke.yml"
  [[ -f "$compose" ]] || die "missing $compose"
  if [[ "${PP_LEDGER_SMOKE_IMAGE_RUN:-0}" != "1" ]]; then
    echo -e "${YELLOW}SKIPPED: image suite scaffold only.${NC}"
    echo "  Compose file: $compose"
    echo "  Set PP_LEDGER_SMOKE_IMAGE_RUN=1 after filling Amp multiaddrs / peer IDs."
    echo "  See deploy/configs/smoke/README.md and docs/ops/TEST_STRATEGY.md"
    return 0
  fi
  if ! command -v docker >/dev/null 2>&1; then
    die "docker required for --suite image"
  fi
  (
    cd "${ROOT}/deploy"
    docker compose -f compose.smoke.yml config >/dev/null
    echo -e "${GREEN}✓ compose.smoke.yml validates${NC}"
    echo -e "${YELLOW}Full image bring-up is still manual (Amp multiaddrs after init).${NC}"
  )
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
