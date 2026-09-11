# Shared helpers for pp-ledger multi-process smoke (source only).
# Ports: smoke profile 8617+ (avoids dogfood 8517+).
# Dial: ADP multiaddrs via pp-client --host (see docs/amp-transport.md).

: "${PP_LEDGER_SMOKE_LIB_LOADED:=}"
if [[ -n "${PP_LEDGER_SMOKE_LIB_LOADED}" ]]; then
  return 0 2>/dev/null || true
fi
PP_LEDGER_SMOKE_LIB_LOADED=1

PP_LEDGER_SMOKE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${PP_LEDGER_BUILD_DIR:-${BUILD_DIR:-${PP_LEDGER_SMOKE_ROOT}/build}}"
TEST_DIR="${PP_LEDGER_SMOKE_DIR:-${BUILD_DIR}/test-smoke}"
PID_FILE="${TEST_DIR}/network.pids"
MULTIADDR_DIR="${TEST_DIR}/multiaddrs"

NUM_MINERS="${PP_LEDGER_SMOKE_MINERS:-3}"
BEACON_PORT="${PP_LEDGER_SMOKE_BEACON_PORT:-8617}"
RELAY_PORT="${PP_LEDGER_SMOKE_RELAY_PORT:-8622}"
MINER_BASE_PORT="${PP_LEDGER_SMOKE_MINER_BASE_PORT:-8618}"
HTTP_PORT="${PP_LEDGER_SMOKE_HTTP_PORT:-8680}"

SLOT_DURATION="${PP_LEDGER_SMOKE_SLOT_DURATION:-2}"
SLOTS_PER_EPOCH="${PP_LEDGER_SMOKE_SLOTS_PER_EPOCH:-10}"
CHECKPOINT_MIN_BLOCKS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS:-2}"
CHECKPOINT_MIN_AGE_SECONDS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_AGE_SECONDS:-0}"

LISTEN_HOST="${PP_LEDGER_SMOKE_LISTEN_HOST:-127.0.0.1}"
: "${PP_LEDGER_SMOKE_DEBUG:=--debug}"
DEBUG_FLAG="${PP_LEDGER_SMOKE_DEBUG}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

die() {
  echo -e "${RED}error: $*${NC}" >&2
  exit 1
}

run_cmd() {
  echo -e "${BLUE}Running: $*${NC}"
  "$@"
}

run_bg_cmd() {
  local log_file="$1"
  shift
  echo -e "${BLUE}Background: $* > $log_file${NC}"
  "$@" >"$log_file" 2>&1 &
}

save_pid() {
  local name=$1
  local pid=$2
  mkdir -p "$(dirname "$PID_FILE")"
  echo "$name:$pid" >>"$PID_FILE"
}

remove_pid_name() {
  local name=$1
  [[ -f "$PID_FILE" ]] || return 0
  grep -v "^${name}:" "$PID_FILE" >"${PID_FILE}.tmp" && mv "${PID_FILE}.tmp" "$PID_FILE"
}

verify_build() {
  [[ -d "$BUILD_DIR" ]] || die "Build directory not found: $BUILD_DIR"
  local exe
  for exe in pp-beacon pp-relay pp-miner pp-client; do
    [[ -x "$BUILD_DIR/app/$exe" ]] || die "$exe not found under $BUILD_DIR/app (build first)"
  done
  if ! command -v python3 &>/dev/null; then
    die "python3 required for key/hex helpers"
  fi
}

hex_to_bin_file() {
  local hex=$1
  local out=$2
  python3 -c "import binascii,sys; sys.stdout.buffer.write(binascii.unhexlify(sys.argv[1].strip()))" "$hex" >"$out"
}

write_amp_identity_key() {
  # Write hex private key from pp-client keygen into path (for relay amp identity).
  local out=$1
  mkdir -p "$(dirname "$out")"
  local output hex
  output=$("$BUILD_DIR/app/pp-client" keygen 2>&1) || {
    echo "$output" >&2
    die "pp-client keygen failed"
  }
  hex=$(echo "$output" | grep "Private key" | sed 's/.*: *//' | tr -d ' \n')
  [[ -n "$hex" ]] || die "pp-client keygen produced no private key"
  printf '%s\n' "$hex" >"$out"
}

stop_network() {
  if [[ ! -f "$PID_FILE" ]]; then
    return 0
  fi
  echo -e "${YELLOW}Stopping smoke network...${NC}"
  while IFS=: read -r name pid; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done <"$PID_FILE"
  sleep 2
  while IFS=: read -r name pid; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
  done <"$PID_FILE"
  rm -f "$PID_FILE"
  echo -e "${GREEN}✓ Network stopped${NC}"
}

clear_network() {
  stop_network
  if [[ -d "$TEST_DIR" ]]; then
    rm -rf "$TEST_DIR"
    echo -e "${GREEN}✓ Smoke data cleared ($TEST_DIR)${NC}"
  else
    echo -e "${BLUE}No smoke data to clear${NC}"
  fi
}

wait_for_listen_multiaddr() {
  local log_file=$1
  local out_file=$2
  local max_wait=${3:-30}
  local elapsed=0
  local line=""
  while [[ $elapsed -lt $max_wait ]]; do
    if [[ -f "$log_file" ]]; then
      # || true: grep miss must not abort under set -e / pipefail
      line=$(grep -aoE 'AMP ledger listener: /[^[:space:]]+' "$log_file" 2>/dev/null | tail -1 | sed 's/^AMP ledger listener: //' || true)
      if [[ -n "$line" && "$line" == /* ]]; then
        # Bind-any is not dialable from the client host.
        line=${line//\/ip4\/0.0.0.0\//\/ip4\/127.0.0.1\/}
        mkdir -p "$(dirname "$out_file")"
        printf '%s\n' "$line" >"$out_file"
        echo -e "${GREEN}✓ Listen multiaddr: $line${NC}"
        return 0
      fi
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  echo -e "${RED}Timed out waiting for AMP ledger listener in $log_file${NC}" >&2
  [[ -f "$log_file" ]] && tail -40 "$log_file" >&2 || true
  return 1
}

beacon_multiaddr() {
  cat "${MULTIADDR_DIR}/beacon" 2>/dev/null || true
}

relay_multiaddr() {
  cat "${MULTIADDR_DIR}/relay" 2>/dev/null || true
}

miner_multiaddr() {
  local id=$1
  cat "${MULTIADDR_DIR}/miner${id}" 2>/dev/null || true
}

# ---- Beacon init / configs ----

initialize_beacon_with_test_config() {
  local beacon_dir="${TEST_DIR}/beacon"
  rm -rf "$beacon_dir"
  mkdir -p "$beacon_dir"
  touch "$beacon_dir/.signature"
  cat >"$beacon_dir/init-config.json" <<EOF
{
  "slotDuration": ${SLOT_DURATION},
  "slotsPerEpoch": ${SLOTS_PER_EPOCH},
  "maxCustomMetaSize": 10000,
  "maxTransactionsPerBlock": 100,
  "minFeeCoefficients": [1, 1, 0],
  "freeCustomMetaSize": 1024,
  "checkpointMinBlocks": ${CHECKPOINT_MIN_BLOCKS},
  "checkpointMinAgeSeconds": ${CHECKPOINT_MIN_AGE_SECONDS}
}
EOF

  echo -e "${CYAN}Created init-config.json (slot=${SLOT_DURATION}s, epoch=${SLOTS_PER_EPOCH})${NC}"

  local init_output
  init_output=$("$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" --init 2>&1) || {
    echo "$init_output" >&2
    die "beacon --init failed"
  }
  # Avoid SIGPIPE under pipefail: print a short prefix without head closing early.
  printf '%s\n' "${init_output}" | awk 'NR<=3 {print}' || true
  echo -e "${GREEN}✓ Beacon initialized${NC}"

  local key_dir="${TEST_DIR}/keys"
  mkdir -p "$key_dir"
  local json_file="${TEST_DIR}/keys_init.json"
  echo "$init_output" >"$json_file"
  if python3 - "$key_dir" "$json_file" <<'PYEOF'
import json, sys, os, re
key_dir, init_path = sys.argv[1], sys.argv[2]
with open(init_path) as f:
    text = f.read()
# Beacon prints: Please save the private keys, they are not recoverable: { ... }
m = re.search(r'not recoverable:\s*(\{[\s\S]*\})\s*$', text)
if not m:
    m = re.search(r'recoverable:\s*(\{[\s\S]*\})', text)
if not m:
    sys.exit(1)
j = json.loads(m.group(1))
for name in ("reserve", "fee", "recycle"):
    arr = j.get(name, [])
    if len(arr) != 3:
        sys.exit(1)
    for i, kp in enumerate(arr, 1):
        pk = kp.get("privateKey", "")
        if not pk:
            sys.exit(1)
        with open(os.path.join(key_dir, f"{name}{i}.key"), "w") as f:
            f.write(pk)
PYEOF
  then
    echo -e "${CYAN}Saved reserve/fee/recycle keys from beacon init${NC}"
  else
    echo -e "${YELLOW}Could not parse init keys (tx inject may be limited)${NC}"
  fi
  rm -f "$json_file"
}

create_beacon_config() {
  local beacon_dir="${TEST_DIR}/beacon"
  cat >"$beacon_dir/config.json" <<EOF
{
  "host": "${LISTEN_HOST}",
  "port": ${BEACON_PORT}
}
EOF
}

create_relay_config() {
  local beacon_ma
  beacon_ma=$(beacon_multiaddr)
  [[ -n "$beacon_ma" ]] || die "beacon multiaddr missing; start beacon first"
  local relay_dir="${TEST_DIR}/relay"
  mkdir -p "$relay_dir/keys"
  if [[ ! -f "$relay_dir/keys/amp-identity.txt" ]]; then
    write_amp_identity_key "$relay_dir/keys/amp-identity.txt"
  fi
  if [[ ! -f "$relay_dir/.signature" ]]; then
    touch "$relay_dir/.signature"
  fi
  cat >"$relay_dir/config.json" <<EOF
{
  "port": ${RELAY_PORT},
  "keys": ["keys/amp-identity.txt"],
  "beacon": "${beacon_ma}"
}
EOF
}

generate_miner_key() {
  local miner_id=$1
  local key_dir="${TEST_DIR}/keys"
  local key_file="${key_dir}/miner${miner_id}.key"
  mkdir -p "$key_dir"
  if [[ ! -f "$key_file" ]]; then
    local output hex
    output=$("$BUILD_DIR/app/pp-client" keygen 2>&1) || {
      echo "$output" >&2
      die "pp-client keygen failed"
    }
    hex=$(echo "$output" | grep "Private key" | sed 's/.*: *//' | tr -d ' \n')
    hex_to_bin_file "$hex" "$key_file"
  fi
  echo "$key_file"
}

create_miner_config() {
  local miner_id=$1
  local miner_dir=$2
  local miner_port=$3
  local key_dir="${TEST_DIR}/keys"
  local relay_ma
  relay_ma=$(relay_multiaddr)
  [[ -n "$relay_ma" ]] || die "relay multiaddr missing; start relay first"

  local keys_json
  if [[ "$miner_id" -eq 1 ]] &&
    [[ -f "$key_dir/fee1.key" && -f "$key_dir/fee2.key" && -f "$key_dir/fee3.key" ]]; then
    cp "$key_dir/fee1.key" "$key_dir/fee2.key" "$key_dir/fee3.key" "$miner_dir/"
    keys_json='["fee1.key", "fee2.key", "fee3.key"]'
    echo -e "${CYAN}Miner 1 using 3 fee keys${NC}"
  elif [[ "$miner_id" -eq 2 ]] &&
    [[ -f "$key_dir/reserve1.key" && -f "$key_dir/reserve2.key" && -f "$key_dir/reserve3.key" ]]; then
    cp "$key_dir/reserve1.key" "$key_dir/reserve2.key" "$key_dir/reserve3.key" "$miner_dir/"
    keys_json='["reserve1.key", "reserve2.key", "reserve3.key"]'
    echo -e "${CYAN}Miner 2 using 3 reserve keys${NC}"
  elif [[ "$miner_id" -eq 3 ]] &&
    [[ -f "$key_dir/recycle1.key" && -f "$key_dir/recycle2.key" && -f "$key_dir/recycle3.key" ]]; then
    cp "$key_dir/recycle1.key" "$key_dir/recycle2.key" "$key_dir/recycle3.key" "$miner_dir/"
    keys_json='["recycle1.key", "recycle2.key", "recycle3.key"]'
    echo -e "${CYAN}Miner 3 using 3 recycle keys${NC}"
  else
    cp "$(generate_miner_key "$miner_id")" "$miner_dir/key.txt"
    keys_json='["key.txt"]'
  fi

  cat >"$miner_dir/config.json" <<EOF
{
  "minerId": ${miner_id},
  "keys": ${keys_json},
  "host": "${LISTEN_HOST}",
  "port": ${miner_port},
  "beacons": ["${relay_ma}"]
}
EOF
}

pid_for_name() {
  local name=$1
  grep "^${name}:" "$PID_FILE" 2>/dev/null | cut -d: -f2 | tail -1
}

# Listen multiaddr is logged before onStart finishes (relay sync / miner dial).
# Wait briefly and require the process to still be alive.
require_process_alive() {
  local name=$1
  local log_file=$2
  local grace_sec=${3:-3}
  local pid
  pid=$(pid_for_name "$name")
  [[ -n "$pid" ]] || die "$name: missing PID"
  sleep "$grace_sec"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo -e "${RED}$name exited after start (PID $pid)${NC}" >&2
    [[ -f "$log_file" ]] && tail -c 4000 "$log_file" | tr -cd '\11\12\15\40-\176\n' | tail -40 >&2 || true
    return 1
  fi
  return 0
}

start_beacon() {
  local beacon_dir="${TEST_DIR}/beacon"
  run_bg_cmd "$beacon_dir/console.log" "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" ${DEBUG_FLAG}
  save_pid "beacon" $!
  wait_for_listen_multiaddr "$beacon_dir/console.log" "${MULTIADDR_DIR}/beacon" 45 || {
    stop_network
    die "beacon failed to publish listen multiaddr"
  }
  require_process_alive "beacon" "$beacon_dir/console.log" 1 || {
    stop_network
    die "beacon process died"
  }
  echo -e "${GREEN}✓ Beacon started on UDP ${BEACON_PORT}${NC}"
}

start_relay() {
  create_relay_config
  local relay_dir="${TEST_DIR}/relay"
  run_bg_cmd "$relay_dir/console.log" "$BUILD_DIR/app/pp-relay" -d "$relay_dir" ${DEBUG_FLAG}
  save_pid "relay" $!
  wait_for_listen_multiaddr "$relay_dir/console.log" "${MULTIADDR_DIR}/relay" 45 || {
    stop_network
    die "relay failed to publish listen multiaddr"
  }
  # Relay logs listen multiaddr before beacon sync; sync failure exits the process.
  require_process_alive "relay" "$relay_dir/console.log" 12 || {
    stop_network
    die "relay exited (often Amp dial/sync to beacon — see console.log)"
  }
  echo -e "${GREEN}✓ Relay started on UDP ${RELAY_PORT}${NC}"
}

start_miner() {
  local miner_id=$1
  local miner_dir="${TEST_DIR}/miner${miner_id}"
  local miner_port=$((MINER_BASE_PORT + miner_id - 1))
  mkdir -p "$miner_dir"
  [[ -f "$miner_dir/.signature" ]] || touch "$miner_dir/.signature"
  create_miner_config "$miner_id" "$miner_dir" "$miner_port"
  run_bg_cmd "${miner_dir}/console.log" "$BUILD_DIR/app/pp-miner" -d "$miner_dir" ${DEBUG_FLAG}
  save_pid "miner${miner_id}" $!
  wait_for_listen_multiaddr "${miner_dir}/console.log" "${MULTIADDR_DIR}/miner${miner_id}" 60 || {
    echo -e "${RED}Miner${miner_id} failed to publish listen multiaddr${NC}" >&2
    cat "${miner_dir}/console.log" >&2 || true
    return 1
  }
  # Miner logs listen before upstream connect; dial failure exits.
  require_process_alive "miner${miner_id}" "${miner_dir}/console.log" 12 || {
    echo -e "${RED}Miner${miner_id} exited after start (often Amp dial to relay)${NC}" >&2
    return 1
  }
  echo -e "${GREEN}✓ Miner${miner_id} started on UDP ${miner_port}${NC}"
}

start_all_miners() {
  local i
  for i in $(seq 1 "$NUM_MINERS"); do
    start_miner "$i" || {
      stop_network
      die "failed to start miner${i}"
    }
  done
}

start_http() {
  if [[ ! -x "$BUILD_DIR/app/pp-http" ]]; then
    echo -e "${YELLOW}pp-http not built; skipping HTTP${NC}"
    return 0
  fi
  local beacon_ma miner_ma
  beacon_ma=$(relay_multiaddr)
  miner_ma=$(miner_multiaddr 1)
  [[ -n "$beacon_ma" && -n "$miner_ma" ]] || die "need relay+miner1 multiaddrs for pp-http"
  local http_dir="${TEST_DIR}/http"
  mkdir -p "$http_dir"
  run_bg_cmd "$http_dir/console.log" "$BUILD_DIR/app/pp-http" --port "$HTTP_PORT" \
    --beacon "$beacon_ma" \
    --miner "$miner_ma"
  save_pid "http" $!
  sleep 1
  if ! kill -0 "$(grep '^http:' "$PID_FILE" | cut -d: -f2)" 2>/dev/null; then
    cat "$http_dir/console.log" >&2 || true
    die "HTTP server failed to start"
  fi
  echo -e "${GREEN}✓ HTTP API on ${HTTP_PORT}${NC}"
}

network_up() {
  verify_build
  if [[ -f "$PID_FILE" ]]; then
    die "smoke network already running (PID file $PID_FILE); stop/clear first"
  fi
  mkdir -p "$TEST_DIR" "$MULTIADDR_DIR"
  initialize_beacon_with_test_config
  create_beacon_config
  start_beacon
  start_relay
  start_all_miners
  start_http
  echo -e "${GREEN}✓ Smoke network up (beacon→relay→miners)${NC}"
}

network_status() {
  echo -e "${BLUE}═══ Smoke network status ═══${NC}"
  if [[ ! -f "$PID_FILE" ]]; then
    echo -e "${YELLOW}Not running (no PID file)${NC}"
    return 0
  fi
  local running=0 stopped=0
  while IFS=: read -r name pid; do
    if kill -0 "$pid" 2>/dev/null; then
      echo -e "${GREEN}✓ $name (PID $pid)${NC}"
      running=$((running + 1))
    else
      echo -e "${RED}✗ $name (PID $pid) stopped${NC}"
      stopped=$((stopped + 1))
    fi
  done <"$PID_FILE"
  echo "Processes: $running running, $stopped stopped"
  local ma
  ma=$(relay_multiaddr)
  if [[ -n "$ma" ]]; then
    if run_pp_client -b --host "$ma" status &>/dev/null; then
      echo -e "${GREEN}✓ pp-client beacon(via relay) reachable${NC}"
    else
      echo -e "${YELLOW}pp-client beacon(via relay) not reachable yet${NC}"
    fi
  fi
  local i port_ma
  for i in $(seq 1 "$NUM_MINERS"); do
    port_ma=$(miner_multiaddr "$i")
    [[ -n "$port_ma" ]] || continue
    if run_pp_client -m --host "$port_ma" status &>/dev/null; then
      echo -e "${GREEN}✓ pp-client miner${i} reachable${NC}"
    else
      echo -e "${YELLOW}pp-client miner${i} not reachable${NC}"
    fi
  done
}

# ---- Client helpers (hard fail) ----

# Bound pp-client so a stuck Amp dial cannot hang the smoke forever.
PP_CLIENT_TIMEOUT_SEC="${PP_LEDGER_SMOKE_CLIENT_TIMEOUT_SEC:-20}"

run_pp_client() {
  if command -v timeout >/dev/null 2>&1; then
    timeout "${PP_CLIENT_TIMEOUT_SEC}" "$BUILD_DIR/app/pp-client" "$@"
  else
    "$BUILD_DIR/app/pp-client" "$@"
  fi
}

fetch_beacon_state() {
  local ma
  ma=$(relay_multiaddr)
  [[ -n "$ma" ]] || die "relay multiaddr missing"
  local output
  output=$(run_pp_client ${DEBUG_FLAG} -b --host "$ma" status 2>&1) || {
    echo -e "${RED}pp-client beacon status failed:${NC}" >&2
    echo "$output" >&2
    return 1
  }
  echo "$output"
}

fetch_miner_status() {
  local miner_id_or_ma=$1
  local ma=""
  if [[ "$miner_id_or_ma" == /* ]]; then
    ma=$miner_id_or_ma
  else
    ma=$(miner_multiaddr "$miner_id_or_ma")
  fi
  [[ -n "$ma" ]] || die "miner multiaddr missing ($miner_id_or_ma)"
  local output
  output=$(run_pp_client ${DEBUG_FLAG} -m --host "$ma" status 2>&1) || {
    echo -e "${RED}pp-client miner status failed:${NC}" >&2
    echo "$output" >&2
    return 1
  }
  echo "$output"
}

parse_next_block_id() {
  local text=$1
  echo "$text" | grep -o '"nextBlockId"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' | head -1
}

get_next_block_id() {
  local out
  out=$(fetch_beacon_state) || return 1
  local id
  id=$(parse_next_block_id "$out")
  [[ -n "$id" ]] || die "could not parse nextBlockId from beacon status"
  echo "$id"
}

get_miner_next_block_id() {
  local miner_id=$1
  local out
  out=$(fetch_miner_status "$miner_id") || return 1
  local id
  id=$(parse_next_block_id "$out")
  [[ -n "$id" ]] || die "could not parse nextBlockId from miner${miner_id}"
  echo "$id"
}

wait_for_beacon_rpc() {
  local max_wait=${1:-45}
  local elapsed=0
  echo -e "${CYAN}Waiting for beacon RPC via relay (max ${max_wait}s)...${NC}"
  while [[ $elapsed -lt $max_wait ]]; do
    if fetch_beacon_state &>/dev/null; then
      echo -e "${GREEN}✓ Beacon RPC ready${NC}"
      return 0
    fi
    sleep 2
    elapsed=$((elapsed + 2))
  done
  die "beacon RPC not ready within ${max_wait}s"
}

wait_for_miner_ready() {
  local max_wait=${1:-45}
  local elapsed=0
  echo -e "${CYAN}Waiting for a miner RPC (max ${max_wait}s)...${NC}" >&2
  while [[ $elapsed -lt $max_wait ]]; do
    local i
    for i in $(seq 1 "$NUM_MINERS"); do
      if fetch_miner_status "$i" &>/dev/null; then
        echo -e "${GREEN}✓ Miner${i} ready${NC}" >&2
        echo "$i"
        return 0
      fi
    done
    sleep 2
    elapsed=$((elapsed + 2))
  done
  die "no miner RPC ready within ${max_wait}s"
}

wait_for_blocks() {
  local target=$1
  local max_wait=${2:-90}
  local elapsed=0
  echo -e "${CYAN}Waiting for nextBlockId >= $target (max ${max_wait}s)...${NC}"
  while [[ $elapsed -lt $max_wait ]]; do
    local next
    next=$(get_next_block_id) || true
    if [[ -n "$next" && "$next" -ge "$target" ]] 2>/dev/null; then
      echo -e "${GREEN}✓ Reached nextBlockId=$next${NC}"
      return 0
    fi
    sleep 2
    elapsed=$((elapsed + 2))
  done
  echo -e "${RED}Timeout: nextBlockId=$(get_next_block_id 2>/dev/null || echo '?') (target $target)${NC}" >&2
  return 1
}

wait_for_tip_increase() {
  local start_tip=$1
  local max_wait=${2:-120}
  local target=$((start_tip + 1))
  wait_for_blocks "$target" "$max_wait"
}

wait_for_miner_tip() {
  local miner_id=$1
  local target=$2
  local max_wait=${3:-90}
  local elapsed=0
  echo -e "${CYAN}Waiting for miner${miner_id} nextBlockId >= $target (max ${max_wait}s)...${NC}"
  while [[ $elapsed -lt $max_wait ]]; do
    local got
    got=$(get_miner_next_block_id "$miner_id" 2>/dev/null) || got=""
    if [[ -n "$got" && "$got" -ge "$target" ]] 2>/dev/null; then
      echo -e "${GREEN}✓ Miner${miner_id} tip=$got (target >= $target)${NC}"
      return 0
    fi
    sleep 2
    elapsed=$((elapsed + 2))
  done
  echo -e "${RED}✗ Miner${miner_id} tip timeout (got=$(get_miner_next_block_id "$miner_id" 2>/dev/null || echo '?'), target=$target)${NC}" >&2
  return 1
}

generate_tx_keypair() {
  local name=$1
  local key_dir="${TEST_DIR}/keys"
  local key_file="${key_dir}/${name}.key"
  local pub_file="${key_dir}/${name}.pub"
  mkdir -p "$key_dir"
  if [[ ! -f "$key_file" ]]; then
    local output
    output=$("$BUILD_DIR/app/pp-client" keygen 2>&1) || {
      echo "$output" >&2
      die "pp-client keygen failed"
    }
    echo "$output" | grep "Private key" | sed 's/.*: *//' | tr -d ' \n' >"$key_file"
    echo "$output" | grep "Public key" | sed 's/.*: *//' | tr -d ' \n' >"$pub_file"
  fi
  echo "$key_file"
}

try_add_account() {
  local to=$1
  local amount=$2
  local new_pubkey_hex=$3
  shift 3
  local key1=$1 key2=$2 key3=$3
  local miner_id=${4:-1}
  local fee=${5:-1}
  local tx_file="${TEST_DIR}/tmp_account_${to}.dat"
  local ma
  ma=$(miner_multiaddr "$miner_id")
  [[ -n "$ma" ]] || return 1
  [[ -f "$key1" && -f "$key2" && -f "$key3" ]] || return 1

  local mk_cmd=(run_pp_client ${DEBUG_FLAG} mk-account 2 "$amount" -t "$to" -f "$fee" -o "$tx_file")
  [[ -n "$new_pubkey_hex" ]] && mk_cmd+=(--new-pubkey "$new_pubkey_hex")
  "${mk_cmd[@]}" >/dev/null 2>&1 || return 1
  local k
  for k in "$key1" "$key2" "$key3"; do
    run_pp_client ${DEBUG_FLAG} sign-tx "$tx_file" -k "$k" >/dev/null 2>&1 || {
      rm -f "$tx_file"
      return 1
    }
  done
  local i
  for i in $(seq 1 "$NUM_MINERS"); do
    ma=$(miner_multiaddr "$i")
    [[ -n "$ma" ]] || continue
    if run_pp_client ${DEBUG_FLAG} -m --host "$ma" submit-tx "$tx_file" >/dev/null 2>&1; then
      echo -e "${GREEN}  ✓ Account created: id=$to amount=$amount${NC}"
      rm -f "$tx_file"
      return 0
    fi
  done
  rm -f "$tx_file"
  return 1
}

try_add_tx() {
  local from=$1
  local to=$2
  local amount=$3
  local key_file=$4
  local fee=${5:-1}
  local miner_id=${6:-1}
  [[ -f "$key_file" ]] || return 1
  local i ma
  for i in "$miner_id" $(seq 1 "$NUM_MINERS"); do
    ma=$(miner_multiaddr "$i")
    [[ -n "$ma" ]] || continue
    if run_pp_client ${DEBUG_FLAG} -m --host "$ma" add-tx "$from" "$to" "$amount" -f "$fee" -k "$key_file" >/dev/null 2>&1; then
      echo -e "${GREEN}  ✓ Tx $from → $to amount=$amount (miner${i})${NC}"
      return 0
    fi
  done
  return 1
}

inject_transactions_for_block_production() {
  echo -e "${BLUE}═══ Injecting transactions to prime block production ═══${NC}"
  local miner_id
  miner_id=$(wait_for_miner_ready 45)

  local alice=$((1 << 30))
  local bob=$(((1 << 30) + 1))
  local key_dir="${TEST_DIR}/keys"
  local reserve1="${key_dir}/reserve1.key" reserve2="${key_dir}/reserve2.key" reserve3="${key_dir}/reserve3.key"
  local min_fee=1

  generate_tx_keypair "alice" >/dev/null
  generate_tx_keypair "bob" >/dev/null
  local alice_key="${key_dir}/alice.key" alice_pub="${key_dir}/alice.pub"
  local bob_key="${key_dir}/bob.key" bob_pub="${key_dir}/bob.pub"
  local alice_pub_hex bob_pub_hex
  alice_pub_hex=$(cat "$alice_pub" 2>/dev/null)
  bob_pub_hex=$(cat "$bob_pub" 2>/dev/null)

  try_add_account "$alice" 10000 "$alice_pub_hex" "$reserve1" "$reserve2" "$reserve3" "$miner_id" $min_fee || true
  try_add_account "$bob" 5000 "$bob_pub_hex" "$reserve1" "$reserve2" "$reserve3" "$miner_id" $min_fee || true
  wait_for_blocks 2 60 || true

  local amt
  for amt in 100 50 25 20 15 10 10 10 5 5 5; do
    try_add_tx "$alice" "$bob" "$amt" "$alice_key" $min_fee "$miner_id" || true
  done
  try_add_tx "$bob" "$alice" 25 "$bob_key" $min_fee "$miner_id" || true
  echo -e "${GREEN}✓ Transaction inject attempted${NC}"
}

stop_miner_by_id() {
  local miner_id=$1
  local line pid
  line=$(grep "^miner${miner_id}:" "$PID_FILE" 2>/dev/null || true)
  [[ -n "$line" ]] || return 0
  pid=$(echo "$line" | cut -d: -f2)
  if kill -0 "$pid" 2>/dev/null; then
    echo -e "${CYAN}Stopping miner${miner_id}...${NC}"
    kill "$pid" 2>/dev/null || true
    sleep 2
  fi
  remove_pid_name "miner${miner_id}"
}

run_latejoin_scenario() {
  echo -e "${BLUE}═══ L-SMOKE-LATEJOIN ═══${NC}"
  wait_for_blocks 2 60 || true
  local next_before
  next_before=$(get_next_block_id)
  echo -e "${CYAN}Current nextBlockId=$next_before${NC}"

  stop_miner_by_id 3
  echo -e "${CYAN}Letting chain progress without miner3...${NC}"
  sleep $((SLOT_DURATION * 3))
  inject_transactions_for_block_production || true
  wait_for_blocks $((next_before + 1)) 90 || true

  echo -e "${CYAN}Restarting miner3...${NC}"
  start_miner 3 || die "failed to restart miner3"
  sleep 4

  local beacon_tip miner_tip
  beacon_tip=$(get_next_block_id)
  if ! wait_for_miner_tip 3 "$beacon_tip" 90; then
    fetch_miner_status 3 2>/dev/null | head -20 || true
    fetch_beacon_state 2>/dev/null | head -20 || true
    die "L-SMOKE-LATEJOIN: miner3 tip did not reach beacon nextBlockId=$beacon_tip"
  fi
  miner_tip=$(get_miner_next_block_id 3)
  if [[ "$miner_tip" -lt "$beacon_tip" ]]; then
    die "L-SMOKE-LATEJOIN tip mismatch: miner=$miner_tip beacon=$beacon_tip"
  fi
  echo -e "${GREEN}✓ L-SMOKE-LATEJOIN: miner3 nextBlockId=$miner_tip >= beacon=$beacon_tip${NC}"
}
