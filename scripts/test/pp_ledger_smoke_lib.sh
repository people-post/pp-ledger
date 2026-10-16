# Shared helpers for pp-ledger multi-process smoke (source only).
# Ports: smoke profile 8617+ (avoids dogfood 8517+).
# Dial: ADP multiaddrs via pp-client --host (see docs/contracts/AMP_TRANSPORT.md).

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
ARTIFACTS_DIR="${TEST_DIR}/artifacts"
FIXTURES_DIR="${PP_LEDGER_SMOKE_FIXTURES:-${PP_LEDGER_SMOKE_ROOT}/scripts/test/fixtures}"
# Amp identities / generated miner keys survive stop and clear (peer IDs stay stable).
LAB_KEYS_DIR="${PP_LEDGER_SMOKE_LAB_KEYS:-${BUILD_DIR}/test-smoke-lab-keys}"

NUM_MINERS="${PP_LEDGER_SMOKE_MINERS:-3}"
BEACON_PORT="${PP_LEDGER_SMOKE_BEACON_PORT:-8617}"
RELAY_PORT="${PP_LEDGER_SMOKE_RELAY_PORT:-8622}"
# A second relay, started by scenarios that need one (start_relay relay2 "$RELAY2_PORT").
RELAY2_PORT="${PP_LEDGER_SMOKE_RELAY2_PORT:-8623}"
# A third relay (the takeover drill's gateway in front of the new beacon).
RELAY3_PORT="${PP_LEDGER_SMOKE_RELAY3_PORT:-8624}"
MINER_BASE_PORT="${PP_LEDGER_SMOKE_MINER_BASE_PORT:-8618}"
HTTP_PORT="${PP_LEDGER_SMOKE_HTTP_PORT:-8680}"

SLOT_DURATION="${PP_LEDGER_SMOKE_SLOT_DURATION:-2}"
# Genesis network id; offline-signed transactions (sign-tx) must name it.
SMOKE_NETWORK_ID="${PP_LEDGER_SMOKE_NETWORK_ID:-pp-smoke}"
SLOTS_PER_EPOCH="${PP_LEDGER_SMOKE_SLOTS_PER_EPOCH:-10}"
CHECKPOINT_MIN_BLOCKS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_BLOCKS:-2}"
CHECKPOINT_MIN_AGE_SECONDS="${PP_LEDGER_SMOKE_CHECKPOINT_MIN_AGE_SECONDS:-0}"

: "${PP_LEDGER_SMOKE_DEBUG:=--debug}"
DEBUG_FLAG="${PP_LEDGER_SMOKE_DEBUG}"

# ---- Fixtures / artifacts / lab keys ----

render_fixture() {
  local tmpl_name=$1
  local out_path=$2
  shift 2
  local tmpl="${FIXTURES_DIR}/${tmpl_name}"
  [[ -f "$tmpl" ]] || die "fixture missing: $tmpl"
  mkdir -p "$(dirname "$out_path")"
  python3 - "$tmpl" "$out_path" "$@" <<'PY'
import sys
path_in, path_out = sys.argv[1], sys.argv[2]
text = open(path_in, encoding="utf-8").read()
for arg in sys.argv[3:]:
    key, _, val = arg.partition("=")
    text = text.replace("__" + key + "__", val)
open(path_out, "w", encoding="utf-8").write(text)
PY
}

lab_keys_wipe() {
  if [[ -d "$LAB_KEYS_DIR" ]]; then
    rm -rf "$LAB_KEYS_DIR"
    echo -e "${GREEN}✓ Lab keys cleared ($LAB_KEYS_DIR)${NC}"
  fi
}

persist_lab_key() {
  local src=$1
  local name=$2
  [[ -f "$src" ]] || return 0
  mkdir -p "$LAB_KEYS_DIR"
  cp -f "$src" "${LAB_KEYS_DIR}/${name}"
}

restore_lab_key() {
  local name=$1
  local dest=$2
  [[ -f "${LAB_KEYS_DIR}/${name}" ]] || return 1
  mkdir -p "$(dirname "$dest")"
  cp -f "${LAB_KEYS_DIR}/${name}" "$dest"
  return 0
}

dump_smoke_artifacts() {
  local stamp
  stamp=$(date +%Y%m%d-%H%M%S 2>/dev/null || echo "fail")
  local dest="${ARTIFACTS_DIR}/${stamp}"
  mkdir -p "$dest"
  [[ -f "$PID_FILE" ]] && cp -f "$PID_FILE" "$dest/network.pids" 2>/dev/null || true
  [[ -d "$MULTIADDR_DIR" ]] && cp -a "$MULTIADDR_DIR" "$dest/multiaddrs" 2>/dev/null || true
  local d
  for d in beacon relay http; do
    [[ -f "${TEST_DIR}/${d}/console.log" ]] && cp -f "${TEST_DIR}/${d}/console.log" "$dest/${d}.console.log" 2>/dev/null || true
    [[ -f "${TEST_DIR}/${d}/config.json" ]] && cp -f "${TEST_DIR}/${d}/config.json" "$dest/${d}.config.json" 2>/dev/null || true
  done
  local i
  for i in $(seq 1 "$NUM_MINERS"); do
    [[ -f "${TEST_DIR}/miner${i}/console.log" ]] && cp -f "${TEST_DIR}/miner${i}/console.log" "$dest/miner${i}.console.log" 2>/dev/null || true
    [[ -f "${TEST_DIR}/miner${i}/config.json" ]] && cp -f "${TEST_DIR}/miner${i}/config.json" "$dest/miner${i}.config.json" 2>/dev/null || true
  done
  {
    echo "TEST_DIR=$TEST_DIR"
    echo "LAB_KEYS_DIR=$LAB_KEYS_DIR"
    echo "BUILD_DIR=$BUILD_DIR"
    date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || true
  } >"$dest/meta.txt"
  echo -e "${YELLOW}Smoke artifacts: $dest${NC}" >&2
}

pids_tree_healthy() {
  [[ -f "$PID_FILE" ]] || return 1
  local name pid any=0
  while IFS=: read -r name pid; do
    [[ -n "$name" && -n "$pid" ]] || continue
    any=1
    kill -0 "$pid" 2>/dev/null || return 1
  done <"$PID_FILE"
  [[ "$any" -eq 1 ]] || return 1
  # Core roles must be present and alive.
  local role
  for role in beacon relay; do
    pid=$(pid_for_name "$role" 2>/dev/null || true)
    [[ -n "$pid" ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
  done
  local i found_miner=0
  for i in $(seq 1 "$NUM_MINERS"); do
    pid=$(pid_for_name "miner${i}" 2>/dev/null || true)
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      found_miner=1
      break
    fi
  done
  [[ "$found_miner" -eq 1 ]] || return 1
  return 0
}

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
  local name pid # not the caller's: bash variables are dynamically scoped
  while IFS=: read -r name pid; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done <"$PID_FILE"
  # Give them up to 5 s to exit cleanly (committing their ledgers), then force.
  local i alive
  for i in $(seq 1 50); do
    alive=0
    while IFS=: read -r name pid; do
      kill -0 "$pid" 2>/dev/null && alive=1
    done <"$PID_FILE"
    ((alive == 0)) && break
    sleep 0.1
  done
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
  # Lab Amp identities survive clear by default (stable peer IDs across fleets).
  if [[ "${PP_LEDGER_SMOKE_WIPE_LAB_KEYS:-0}" == "1" ]]; then
    lab_keys_wipe
  fi
}

wait_for_listen_multiaddr() {
  local log_file=$1
  local out_file=$2
  local max_wait=${3:-30}
  local deadline=$((SECONDS + max_wait))
  local line=""
  while ((SECONDS < deadline)); do
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
    sleep 0.2
  done
  echo -e "${RED}Timed out waiting for AMP ledger listener in $log_file${NC}" >&2
  [[ -f "$log_file" ]] && tail -40 "$log_file" >&2 || true
  return 1
}

beacon_multiaddr() {
  cat "${MULTIADDR_DIR}/beacon" 2>/dev/null || true
}

relay_multiaddr() {
  cat "${MULTIADDR_DIR}/${1:-relay}" 2>/dev/null || true
}

miner_multiaddr() {
  local id=$1
  cat "${MULTIADDR_DIR}/miner${id}" 2>/dev/null || true
}

# ---- Beacon init / configs ----

initialize_beacon_with_test_config() {
  local beacon_dir="${TEST_DIR}/beacon"
  rm -rf "$beacon_dir"
  # A new genesis is a new chain: every node's ledger from an earlier run belongs to another
  # chain (relay errors on every request; miners' blocks are refused), and its logs to another
  # network (checks read them). A relay an earlier run promoted to beacon goes whole. Keys and Amp
  # identities stay (the lab keys restore a removed relay's).
  local node_dir
  for node_dir in "${TEST_DIR}"/relay* "${TEST_DIR}"/miner*; do
    [[ -d "$node_dir" ]] || continue
    if [[ -f "$node_dir/config.relay.json" ]]; then
      rm -rf "$node_dir"
      continue
    fi
    rm -rf "$node_dir/data"
    rm -f "$node_dir"/*.log
  done
  mkdir -p "$beacon_dir/keys"
  touch "$beacon_dir/.signature"
  if restore_lab_key "beacon-amp-identity.txt" "$beacon_dir/keys/amp-identity.txt"; then
    echo -e "${CYAN}Restored lab beacon Amp identity${NC}"
  fi
  # Every account holder makes its own keys: 3 per system account (all must
  # sign), one per miner. The beacon only ever sees public keys; the genesis
  # keys sign genesis (--genesis-key). Reserve keys later fund test accounts.
  local key_dir="${TEST_DIR}/keys" name i sep="" system_accounts="" genesis_args=()
  mkdir -p "$key_dir"
  for name in genesis reserve registrar fee recycle; do
    local pubs="" psep=""
    for i in 1 2 3; do
      # A key without its .pub (older layout) cannot be listed in genesis: make a fresh pair.
      if [[ ! -f "$key_dir/${name}${i}.key" || ! -f "$key_dir/${name}${i}.pub" ]]; then
        rm -f "$key_dir/${name}${i}.key"  # keygen will not overwrite
        "$BUILD_DIR/app/pp-client" keygen -o "$key_dir/${name}${i}" >/dev/null || die "pp-client keygen failed"
      fi
      pubs+="${psep}\"$(tr -d ' \n' <"$key_dir/${name}${i}.pub")\""
      psep=", "
    done
    system_accounts+="${sep}\"${name}\": {\"publicKeys\": [${pubs}]}"
    sep=", "
  done
  for i in 1 2 3; do
    genesis_args+=(--genesis-key "$key_dir/genesis${i}.key")
  done

  # Recovery set (beacon-authority): its own holder's key, never an admin key. A short delay keeps
  # it testable with smoke epochs.
  if [[ ! -f "$key_dir/recovery1.key" || ! -f "$key_dir/recovery1.pub" ]]; then
    rm -f "$key_dir/recovery1.key"
    "$BUILD_DIR/app/pp-client" keygen -o "$key_dir/recovery1" >/dev/null || die "pp-client keygen failed"
  fi
  local recovery="{\"publicKeys\": [\"$(tr -d ' \n' <"$key_dir/recovery1.pub")\"], \"delayEpochs\": 2}"

  # Miners are genesis miner accounts (issued range) with their own keys;
  # system accounts never lead slots.
  local genesis_miners=""
  sep=""
  for i in $(seq 1 "$NUM_MINERS"); do
    generate_miner_key "$i" >/dev/null
    genesis_miners+="${sep}{\"id\": $(miner_account_id "$i"), \"publicKeys\": [\"$(cat "${TEST_DIR}/keys/miner${i}.pub")\"]}"
    sep=", "
  done
  render_fixture "init-config.json.tmpl" "$beacon_dir/init-config.json" \
    "SYSTEM_ACCOUNTS={${system_accounts}}" \
    "RECOVERY=${recovery}" \
    "GENESIS_MINERS=[${genesis_miners}]" \
    "NETWORK_ID=${SMOKE_NETWORK_ID}" \
    "SLOT_DURATION=${SLOT_DURATION}" \
    "SLOTS_PER_EPOCH=${SLOTS_PER_EPOCH}" \
    "CHECKPOINT_MIN_BLOCKS=${CHECKPOINT_MIN_BLOCKS}" \
    "CHECKPOINT_MIN_AGE_SECONDS=${CHECKPOINT_MIN_AGE_SECONDS}"

  echo -e "${CYAN}Created init-config.json (slot=${SLOT_DURATION}s, epoch=${SLOTS_PER_EPOCH})${NC}"

  local init_output
  init_output=$("$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" --init "${genesis_args[@]}" 2>&1) || {
    echo "$init_output" >&2
    die "beacon --init failed"
  }
  # Avoid SIGPIPE under pipefail: print a short prefix without head closing early.
  printf '%s\n' "${init_output}" | awk 'NR<=3 {print}' || true
  echo -e "${GREEN}✓ Beacon initialized${NC}"
}

create_beacon_config() {
  local beacon_dir="${TEST_DIR}/beacon"
  render_fixture "beacon-config.json.tmpl" "$beacon_dir/config.json" \
    "BEACON_PORT=${BEACON_PORT}"
}

# Relay $1 (default "relay", in ${TEST_DIR}/$1) on UDP port $2 (default RELAY_PORT), following
# upstream $3 (default the beacon).
create_relay_config() {
  local name=${1:-relay} port=${2:-$RELAY_PORT} beacon_ma=${3:-}
  [[ -n "$beacon_ma" ]] || beacon_ma=$(beacon_multiaddr)
  [[ -n "$beacon_ma" ]] || die "beacon multiaddr missing; start beacon first"
  local relay_dir="${TEST_DIR}/${name}"
  mkdir -p "$relay_dir/keys"
  if [[ ! -f "$relay_dir/keys/amp-identity.txt" ]]; then
    if restore_lab_key "${name}-amp-identity.txt" "$relay_dir/keys/amp-identity.txt"; then
      echo -e "${CYAN}Restored lab ${name} Amp identity${NC}"
    else
      write_amp_identity_key "$relay_dir/keys/amp-identity.txt"
    fi
  fi
  persist_lab_key "$relay_dir/keys/amp-identity.txt" "${name}-amp-identity.txt"
  if [[ ! -f "$relay_dir/.signature" ]]; then
    touch "$relay_dir/.signature"
  fi
  render_fixture "relay-config.json.tmpl" "$relay_dir/config.json" \
    "RELAY_PORT=${port}" \
    "BEACON_MULTIADDR=${beacon_ma}"
  # Optional: pin the genesis the relay syncs from (networkAnchor.genesisHash).
  if [[ "${PP_LEDGER_SMOKE_RELAY_PIN_GENESIS:-0}" == "1" ]]; then
    local hash
    hash=$(genesis_hash_hex "$beacon_ma")
    [[ -n "$hash" ]] || die "could not read the genesis hash from the beacon"
    sed -i "s|^  \"beacon\"|  \"networkAnchor\": {\"genesisHash\": \"${hash}\"},\n  \"beacon\"|" "$relay_dir/config.json"
    grep -q '"networkAnchor"' "$relay_dir/config.json" || die "failed to pin genesis in the relay config"
  fi
}

# Miner N (1-based smoke index) runs as genesis miner account 2^20 + N - 1.
miner_account_id() {
  echo $(((1 << 20) + $1 - 1))
}

# Miner N's own key pair: miner N.key (binary private key) and minerN.pub
# (hex public key, listed in genesisMiners). Kept as lab keys across clears.
generate_miner_key() {
  local miner_id=$1
  local key_dir="${TEST_DIR}/keys"
  local key_file="${key_dir}/miner${miner_id}.key"
  local pub_file="${key_dir}/miner${miner_id}.pub"
  mkdir -p "$key_dir"
  if [[ ! -f "$key_file" || ! -f "$pub_file" ]]; then
    if restore_lab_key "miner${miner_id}.key" "$key_file" && restore_lab_key "miner${miner_id}.pub" "$pub_file"; then
      :
    else
      local output priv pub
      output=$("$BUILD_DIR/app/pp-client" keygen 2>&1) || {
        echo "$output" >&2
        die "pp-client keygen failed"
      }
      priv=$(echo "$output" | grep "Private key" | sed 's/.*: *//' | tr -d ' \n')
      pub=$(echo "$output" | grep "Public key" | sed 's/.*: *//' | tr -d ' \n')
      hex_to_bin_file "$priv" "$key_file"
      printf '%s' "$pub" >"$pub_file"
      persist_lab_key "$key_file" "miner${miner_id}.key"
      persist_lab_key "$pub_file" "miner${miner_id}.pub"
    fi
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

  cp "$(generate_miner_key "$miner_id")" "$miner_dir/key.txt"
  local keys_json='["key.txt"]'

  render_fixture "miner-config.json.tmpl" "$miner_dir/config.json" \
    "MINER_ID=$(miner_account_id "$miner_id")" \
    "KEYS_JSON=${keys_json}" \
    "MINER_PORT=${miner_port}" \
    "RELAY_MULTIADDR=${relay_ma}"
}

pid_for_name() {
  local name=$1
  grep "^${name}:" "$PID_FILE" 2>/dev/null | cut -d: -f2 | tail -1
}

# A node logs this once onStart is done (relay sync, miner dial/register) and
# its run loop is serving: BeaconServer "Request handler thread started",
# RelayServer / MinerServer "... handler loop started".
READY_MARKER='handler (loop|thread) started'

# Poll until the node is ready instead of sleeping a fixed grace; fail fast
# if it exits during startup.
wait_until_ready() {
  local name=$1
  local log_file=$2
  local max_wait=${3:-60}
  local pid deadline=$((SECONDS + max_wait))
  pid=$(pid_for_name "$name")
  [[ -n "$pid" ]] || die "$name: missing PID"
  while ((SECONDS < deadline)); do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo -e "${RED}$name exited during startup (PID $pid)${NC}" >&2
      [[ -f "$log_file" ]] && tail -c 4000 "$log_file" | tr -cd '\11\12\15\40-\176\n' | tail -40 >&2 || true
      return 1
    fi
    if grep -aqE "$READY_MARKER" "$log_file" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done
  echo -e "${RED}$name not ready within ${max_wait}s${NC}" >&2
  [[ -f "$log_file" ]] && tail -c 4000 "$log_file" | tr -cd '\11\12\15\40-\176\n' | tail -40 >&2 || true
  return 1
}

start_beacon() {
  local beacon_dir="${TEST_DIR}/beacon"
  run_bg_cmd "$beacon_dir/console.log" "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" ${DEBUG_FLAG}
  save_pid "beacon" $!
  wait_for_listen_multiaddr "$beacon_dir/console.log" "${MULTIADDR_DIR}/beacon" 45 || {
    stop_network
    die "beacon failed to publish listen multiaddr"
  }
  wait_until_ready "beacon" "$beacon_dir/console.log" 30 || {
    stop_network
    die "beacon process died"
  }
  persist_lab_key "$beacon_dir/keys/amp-identity.txt" "beacon-amp-identity.txt"
  echo -e "${GREEN}✓ Beacon started on UDP ${BEACON_PORT}${NC}"
}

# Start relay $1 (default "relay") on UDP port $2 (default RELAY_PORT), following upstream $3
# (default the beacon); see create_relay_config.
start_relay() {
  local name=${1:-relay} port=${2:-$RELAY_PORT} upstream=${3:-}
  create_relay_config "$name" "$port" "$upstream"
  local relay_dir="${TEST_DIR}/${name}"
  run_bg_cmd "$relay_dir/console.log" "$BUILD_DIR/app/pp-relay" -d "$relay_dir" ${DEBUG_FLAG}
  save_pid "$name" $!
  wait_for_listen_multiaddr "$relay_dir/console.log" "${MULTIADDR_DIR}/${name}" 45 || {
    stop_network
    die "${name} failed to publish listen multiaddr"
  }
  # Relay logs listen multiaddr before beacon sync; sync failure exits the process.
  wait_until_ready "$name" "$relay_dir/console.log" 60 || {
    stop_network
    die "${name} exited (often Amp dial/sync to beacon — see console.log)"
  }
  echo -e "${GREEN}✓ ${name^} started on UDP ${port}${NC}"
}

launch_miner() {
  local miner_id=$1
  local miner_dir="${TEST_DIR}/miner${miner_id}"
  local miner_port=$((MINER_BASE_PORT + miner_id - 1))
  mkdir -p "$miner_dir"
  [[ -f "$miner_dir/.signature" ]] || touch "$miner_dir/.signature"
  create_miner_config "$miner_id" "$miner_dir" "$miner_port"
  run_bg_cmd "${miner_dir}/console.log" "$BUILD_DIR/app/pp-miner" -d "$miner_dir" ${DEBUG_FLAG}
  save_pid "miner${miner_id}" $!
}

await_miner() {
  local miner_id=$1
  local miner_dir="${TEST_DIR}/miner${miner_id}"
  local miner_port=$((MINER_BASE_PORT + miner_id - 1))
  wait_for_listen_multiaddr "${miner_dir}/console.log" "${MULTIADDR_DIR}/miner${miner_id}" 60 || {
    echo -e "${RED}Miner${miner_id} failed to publish listen multiaddr${NC}" >&2
    cat "${miner_dir}/console.log" >&2 || true
    return 1
  }
  # Miner logs listen before upstream connect / register / startup sync.
  wait_until_ready "miner${miner_id}" "${miner_dir}/console.log" 60 || {
    echo -e "${RED}Miner${miner_id} exited or stalled during startup (often Amp dial to relay)${NC}" >&2
    return 1
  }
  echo -e "${GREEN}✓ Miner${miner_id} started on UDP ${miner_port}${NC}"
}

start_miner() {
  launch_miner "$1"
  await_miner "$1"
}

# Launch every miner, then wait for each: they start in parallel.
start_all_miners() {
  local i
  for i in $(seq 1 "$NUM_MINERS"); do
    launch_miner "$i"
  done
  for i in $(seq 1 "$NUM_MINERS"); do
    await_miner "$i" || {
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
  local id=""
  id=$(printf '%s' "$text" | python3 -c '
import json, sys
text = sys.stdin.read()
dec = json.JSONDecoder()
i = 0
last = None
while i < len(text):
    if text[i] == "{":
        try:
            obj, end = dec.raw_decode(text, i)
        except json.JSONDecodeError:
            i += 1
            continue
        if isinstance(obj, dict) and "nextBlockId" in obj:
            last = obj["nextBlockId"]
        i = end
    else:
        i += 1
if last is None:
    sys.exit(1)
print(last)
' 2>/dev/null || true)
  if [[ -z "$id" ]]; then
    # Fallback for non-JSON / partial prints.
    id=$(printf '%s' "$text" | grep -oE '"nextBlockId"[[:space:]]*:[[:space:]]*[0-9]+' | grep -oE '[0-9]+$' | tail -1 || true)
  fi
  printf '%s' "$id"
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
  local deadline=$((SECONDS + max_wait))
  echo -e "${CYAN}Waiting for beacon RPC via relay (max ${max_wait}s)...${NC}"
  while ((SECONDS < deadline)); do
    if fetch_beacon_state &>/dev/null; then
      echo -e "${GREEN}✓ Beacon RPC ready${NC}"
      return 0
    fi
    sleep 0.5
  done
  dump_smoke_artifacts || true
  die "beacon RPC not ready within ${max_wait}s"
}

wait_for_miner_ready() {
  local max_wait=${1:-45}
  local deadline=$((SECONDS + max_wait))
  echo -e "${CYAN}Waiting for a miner RPC (max ${max_wait}s)...${NC}" >&2
  while ((SECONDS < deadline)); do
    local i
    for i in $(seq 1 "$NUM_MINERS"); do
      if fetch_miner_status "$i" &>/dev/null; then
        echo -e "${GREEN}✓ Miner${i} ready${NC}" >&2
        echo "$i"
        return 0
      fi
    done
    sleep 0.5
  done
  dump_smoke_artifacts || true
  die "no miner RPC ready within ${max_wait}s"
}

# ---- L0 ready-gate layers (fail-fast) ----

assert_l0a_pids_alive() {
  echo -e "${CYAN}L0a: process PIDs alive${NC}"
  pids_tree_healthy || {
    dump_smoke_artifacts || true
    die "L0a failed: smoke PID tree unhealthy (beacon/relay/miner)"
  }
  echo -e "${GREEN}✓ L0a: core processes alive${NC}"
}

assert_l0b_beacon_rpc() {
  local timeout_sec=${1:-60}
  echo -e "${CYAN}L0b: beacon RPC via relay${NC}"
  wait_for_beacon_rpc "$timeout_sec"
  local state tip
  state=$(fetch_beacon_state) || {
    dump_smoke_artifacts || true
    die "L0b failed: beacon status RPC"
  }
  tip=$(parse_next_block_id "$state")
  [[ -n "$tip" ]] || {
    dump_smoke_artifacts || true
    die "L0b failed: beacon status missing nextBlockId"
  }
  echo -e "${GREEN}✓ L0b: beacon status OK (nextBlockId=$tip)${NC}"
}

assert_l0c_miner_rpc() {
  local timeout_sec=${1:-60}
  echo -e "${CYAN}L0c: miner RPC${NC}"
  local miner_id mstate mtip
  miner_id=$(wait_for_miner_ready "$timeout_sec")
  mstate=$(fetch_miner_status "$miner_id") || {
    dump_smoke_artifacts || true
    die "L0c failed: miner${miner_id} status RPC"
  }
  mtip=$(parse_next_block_id "$mstate")
  [[ -n "$mtip" ]] || {
    dump_smoke_artifacts || true
    die "L0c failed: miner${miner_id} status missing nextBlockId"
  }
  echo -e "${GREEN}✓ L0c: miner${miner_id} status OK (nextBlockId=$mtip)${NC}"
}

network_ready() {
  local timeout_sec=${1:-60}
  assert_l0a_pids_alive
  assert_l0b_beacon_rpc "$timeout_sec"
  assert_l0c_miner_rpc "$timeout_sec"
  echo -e "${GREEN}✓ network_ready${NC}"
}

wait_for_blocks() {
  local target=$1
  local max_wait=${2:-90}
  local deadline=$((SECONDS + max_wait))
  echo -e "${CYAN}Waiting for nextBlockId >= $target (max ${max_wait}s)...${NC}"
  while ((SECONDS < deadline)); do
    local next
    next=$(get_next_block_id) || true
    if [[ -n "$next" && "$next" -ge "$target" ]] 2>/dev/null; then
      echo -e "${GREEN}✓ Reached nextBlockId=$next${NC}"
      return 0
    fi
    sleep 0.5
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
  local deadline=$((SECONDS + max_wait))
  echo -e "${CYAN}Waiting for miner${miner_id} nextBlockId >= $target (max ${max_wait}s)...${NC}"
  while ((SECONDS < deadline)); do
    local got
    got=$(get_miner_next_block_id "$miner_id" 2>/dev/null) || got=""
    if [[ -n "$got" && "$got" -ge "$target" ]] 2>/dev/null; then
      echo -e "${GREEN}✓ Miner${miner_id} tip=$got (target >= $target)${NC}"
      return 0
    fi
    sleep 0.5
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

# Reserve (system account 1) funds the smoke test accounts.
RESERVE_ACCOUNT_ID=1

try_add_account() {
  local to=$1
  local amount=$2
  local new_pubkey_hex=$3
  shift 3
  local key1=$1 key2=$2 key3=$3
  local miner_id=${4:-1}
  local fee=${5:-1}
  local tx_file="${TEST_DIR}/tmp_account_${to}.dat"
  [[ -f "$key1" && -f "$key2" && -f "$key3" ]] || return 1

  local mk_cmd=(run_pp_client ${DEBUG_FLAG} mk-account "$RESERVE_ACCOUNT_ID" "$amount" -t "$to" -f "$fee" -o "$tx_file")
  [[ -n "$new_pubkey_hex" ]] && mk_cmd+=(--new-pubkey "$new_pubkey_hex")
  "${mk_cmd[@]}" >/dev/null 2>&1 || return 1
  local k
  for k in "$key1" "$key2" "$key3"; do
    run_pp_client ${DEBUG_FLAG} sign-tx "$tx_file" -k "$k" --network-id "$SMOKE_NETWORK_ID" >/dev/null 2>&1 || {
      rm -f "$tx_file"
      return 1
    }
  done
  # Transactions go up the tree: submit to the relay; the slot leader pulls them.
  if run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" submit-tx "$tx_file" >/dev/null 2>&1; then
    echo -e "${GREEN}  ✓ Account submitted: id=$to amount=$amount${NC}"
    rm -f "$tx_file"
    return 0
  fi
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
  if run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" add-tx "$from" "$to" "$amount" -f "$fee" -k "$key_file" >/dev/null 2>&1; then
    echo -e "${GREEN}  ✓ Tx $from → $to amount=$amount${NC}"
    return 0
  fi
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

# Register an account as a miner (T_MINER_REGISTER, signed by the account) and wait until the
# chain shows it registered. The fixture's minerRegistrationFee is 10; overpaying is allowed.
register_miner_account() {
  local account=$1
  local key_file=$2
  local max_wait=${3:-90}
  local fee=20
  echo -e "${CYAN}Registering account $account as a miner...${NC}"
  local deadline=$((SECONDS + max_wait))
  local submitted=0
  while ((SECONDS < deadline)); do
    if ((submitted == 0)) &&
      run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" miner-register "$account" -f "$fee" -k "$key_file" >/dev/null 2>&1; then
      submitted=1
    fi
    local meta
    meta=$(run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" account "$account" 2>/dev/null || true)
    if printf '%s' "$meta" | tr -d ' \n' | grep -q '"miner":{[^}]*"registered":true'; then
      echo -e "${GREEN}✓ Account $account is a registered miner${NC}"
      return 0
    fi
    sleep 1
  done
  echo -e "${RED}✗ Account $account not registered as a miner within ${max_wait}s (submitted=$submitted)${NC}" >&2
  return 1
}

# Stop node $1 (its name in the PID file) and forget its PID.
stop_node() {
  local name=$1
  local pid
  pid=$(pid_for_name "$name")
  [[ -n "$pid" ]] || return 0
  if kill -0 "$pid" 2>/dev/null; then
    echo -e "${CYAN}Stopping ${name}...${NC}"
    kill "$pid" 2>/dev/null || true
    # A clean stop commits the ledger; force it after 5 s so no two processes share the directory.
    local i
    for i in $(seq 1 50); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  fi
  remove_pid_name "$name"
}

stop_miner_by_id() {
  stop_node "miner$1"
}

run_latejoin_scenario() {
  echo -e "${BLUE}═══ L-SMOKE-LATEJOIN ═══${NC}"
  wait_for_blocks 2 60 || true
  local next_before
  next_before=$(get_next_block_id)
  echo -e "${CYAN}Current nextBlockId=$next_before${NC}"

  stop_miner_by_id 3
  echo -e "${CYAN}Letting chain progress without miner3...${NC}"
  inject_transactions_for_block_production || true
  wait_for_blocks $((next_before + 1)) 90 || true

  echo -e "${CYAN}Restarting miner3...${NC}"
  start_miner 3 || die "failed to restart miner3"

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

# --- L-SMOKE-ROTATION (beacon-authority ba5): rotate the beacon signing key on the same server ---

# Stage keys/beacon-signing.next.{key,pub} on the beacon (before it starts: keys load at start).
stage_next_beacon_key() {
  local keys="${TEST_DIR}/beacon/keys"
  rm -f "$keys/beacon-signing.next.key" "$keys/beacon-signing.next.pub"
  "$BUILD_DIR/app/pp-client" keygen -o "$keys/beacon-signing.next" >/dev/null || die "pp-client keygen failed"
  echo -e "${CYAN}Staged the next beacon signing key${NC}"
}

# Beacon generation of the signed tip the relay serves (empty if none).
signed_tip_generation() {
  fetch_beacon_state 2>/dev/null | sed -n 's/^Signed tip: block [0-9]*, generation \([0-9]*\)$/\1/p'
}

# The admins certify generation $1 for the key in $2 (a .pub file), with cutover $3 (none if
# empty — with one, mk-beacon-cert reads the block's hash from the relay) and dial hint $4 (none
# if empty): write the record, sign it offline (genesis is 3-of-3); prints the file.
make_beacon_cert() {
  local generation=$1 pubkey=$2 cutover=${3:-} hint=${4:-} cert="${TEST_DIR}/cert-gen$1.tx" i
  local node_args=() cutover_args=() hint_args=()
  if [[ -n "$cutover" ]]; then
    node_args=(${DEBUG_FLAG} -b --host "$(relay_multiaddr)")
    cutover_args=(--cutover "$cutover")
  fi
  [[ -z "$hint" ]] || hint_args=(--dial-hint "$hint")
  rm -f "$cert"
  run_pp_client ${node_args[@]+"${node_args[@]}"} mk-beacon-cert --network-id "$SMOKE_NETWORK_ID" \
    --generation "$generation" --pubkey "$pubkey" ${cutover_args[@]+"${cutover_args[@]}"} \
    ${hint_args[@]+"${hint_args[@]}"} -o "$cert" >/dev/null ||
    die "mk-beacon-cert failed"
  for i in 1 2 3; do
    run_pp_client sign-tx -k "${TEST_DIR}/keys/genesis${i}.key" --network-id "$SMOKE_NETWORK_ID" "$cert" >/dev/null ||
      die "sign-tx failed"
  done
  echo "$cert"
}

run_rotation_scenario() {
  echo -e "${BLUE}═══ L-SMOKE-ROTATION ═══${NC}"
  wait_for_blocks 2 60 || true
  local generation
  generation=$(signed_tip_generation)
  [[ "$generation" == "1" ]] || die "L-SMOKE-ROTATION: expected generation 1 before rotating, got '${generation}'"

  # The admins certify the staged key as generation 2, with no cutover (the old key is not
  # stolen: it signs until the certificate commits), and submit it through the relay.
  local cert next i
  next=$(get_next_block_id)
  cert=$(make_beacon_cert 2 "${TEST_DIR}/beacon/keys/beacon-signing.next.pub")
  run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" submit-tx "$cert" >/dev/null || die "submit-tx failed"
  echo -e "${CYAN}Submitted the generation-2 certificate (no cutover)${NC}"

  local deadline=$((SECONDS + 90))
  while ((SECONDS < deadline)); do
    generation=$(signed_tip_generation)
    [[ "$generation" == "2" ]] && break
    sleep 1
  done
  [[ "$generation" == "2" ]] || die "L-SMOKE-ROTATION: the relay never served a generation-2 tip (got '${generation}')"
  echo -e "${GREEN}✓ Relay serves a generation-2 signed tip${NC}"

  # The chain keeps moving under the new key, and every miner follows it.
  local after
  after=$(get_next_block_id)
  inject_transactions_for_block_production || true
  wait_for_blocks $((after + 1)) 90 || die "L-SMOKE-ROTATION: no block after the rotation"
  local beacon_tip
  beacon_tip=$(get_next_block_id)
  for i in $(seq 1 "$NUM_MINERS"); do
    wait_for_miner_tip "$i" "$beacon_tip" 90 || die "L-SMOKE-ROTATION: miner${i} did not follow generation 2"
  done
  [[ "$(signed_tip_generation)" == "2" ]] || die "L-SMOKE-ROTATION: generation went back"
  echo -e "${GREEN}✓ Generation 2 signs; miners at nextBlockId >= ${beacon_tip}${NC}"

  # beacon-authority ba5b: once a checkpoint lies above the certificate, fresh miners start there
  # (not at genesis), warm up, and go on to produce. Miner3 pins genesis: the upstream's authority
  # proof (with the generation-2 certificate) must tie the checkpoint to it. Miner2 does not.
  local checkpoint
  checkpoint=$(wait_for_checkpoint_above "$next" 180) || die "L-SMOKE-ROTATION: no checkpoint above block ${next}"
  echo -e "${CYAN}Checkpoint ${checkpoint} lies above the certificate${NC}"
  join_fresh_miner 3 anchored "$checkpoint"
  join_fresh_miner 2 plain "$checkpoint"
  echo -e "${GREEN}✓ L-SMOKE-ROTATION: generation 2 signs; fresh miners start at a checkpoint (pinned and not) and validate past the next${NC}"
}

# --- L-SMOKE-EQUIVOCATION (beacon-authority ba6): two tips at one height halt the network ---

# The equivocation the relay serves on STATUS: "<generation> <height>" (empty if none).
status_equivocation() {
  fetch_beacon_state 2>/dev/null | sed -n 's/^Equivocation: generation \([0-9]*\) at block \([0-9]*\) .*$/\1 \2/p'
}

# Wait up to $2 seconds for file $1 to exist ($3 = 0: to be gone).
wait_for_file() {
  local file=$1 max_wait=$2 present=${3:-1} deadline=$((SECONDS + $2))
  while ((SECONDS < deadline)); do
    if [[ "$present" == "1" && -f "$file" ]] || [[ "$present" == "0" && ! -f "$file" ]]; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

run_equivocation_halt_scenario() {
  echo -e "${BLUE}═══ L-SMOKE-EQUIVOCATION (halt) ═══${NC}"
  wait_for_blocks 2 60 || true
  # A drill: the generation-1 beacon key signs two blocks at the current tip height, as a thief or a
  # second host running the key would. The evidence goes to the relay, like any operator's report.
  local tip evidence reply i
  tip=$(status_signed_tip_height)
  [[ -n "$tip" ]] || die "L-SMOKE-EQUIVOCATION: no signed tip to start from"
  evidence="${TEST_DIR}/equivocation-drill.dat"
  rm -f "$evidence"
  run_pp_client mk-equivocation --network-id "$SMOKE_NETWORK_ID" --generation 1 --height "$tip" \
    -k "${TEST_DIR}/beacon/keys/beacon-signing.key" -o "$evidence" >/dev/null || die "mk-equivocation failed"
  reply=$(run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" add-equivocation "$evidence" 2>&1) ||
    die "L-SMOKE-EQUIVOCATION: add-equivocation failed: ${reply}"
  echo -e "${CYAN}Handed the relay equivocation evidence at block ${tip}: ${reply}${NC}"

  # The relay keeps and serves it; it passed it up, so the beacon halts too.
  [[ "$(status_equivocation)" == "1 ${tip}" ]] ||
    die "L-SMOKE-EQUIVOCATION: the relay does not serve the evidence on STATUS (got '$(status_equivocation)')"
  [[ -f "${TEST_DIR}/relay/data/equivocation.dat" ]] || die "L-SMOKE-EQUIVOCATION: the relay did not persist it"
  wait_for_file "${TEST_DIR}/beacon/data/equivocation.dat" 10 || die "L-SMOKE-EQUIVOCATION: the beacon did not keep it"
  grep -a -q "EQUIVOCATION: beacon generation 1 signed two blocks at height ${tip}" "${TEST_DIR}/beacon/beacon.log" ||
    die "L-SMOKE-EQUIVOCATION: the beacon did not alert"
  # Miners learn it from their upstream's STATUS.
  for i in $(seq 1 "$NUM_MINERS"); do
    wait_for_file "${TEST_DIR}/miner${i}/data/equivocation.dat" 90 ||
      die "L-SMOKE-EQUIVOCATION: miner${i} did not keep the evidence"
  done
  echo -e "${GREEN}✓ Relay, beacon and every miner hold the evidence${NC}"

  local held
  held=$(get_next_block_id)
  inject_transactions_for_block_production >/dev/null 2>&1 || true
  sleep $((SLOT_DURATION * 4))
  [[ "$(get_next_block_id)" == "$held" ]] || die "L-SMOKE-EQUIVOCATION: blocks committed after the halt"
  echo -e "${GREEN}✓ L-SMOKE-EQUIVOCATION: the network halted at nextBlockId ${held}${NC}"
}

# After a later generation is certified (L-SMOKE-ROTATION's generation 2): every node resumed.
check_equivocation_resumed() {
  [[ -z "$(status_equivocation)" ]] || die "L-SMOKE-EQUIVOCATION: the relay still serves the evidence"
  local node
  for node in relay beacon; do
    wait_for_file "${TEST_DIR}/${node}/data/equivocation.dat" 30 0 ||
      die "L-SMOKE-EQUIVOCATION: ${node} still holds the superseded evidence"
  done
  echo -e "${GREEN}✓ L-SMOKE-EQUIVOCATION: generation 2 resumed the network; the evidence is gone${NC}"
}

# --- L-SMOKE-RECOVERY (beacon-authority ba7): the recovery set replaces the admin set ---

# The pending recovery the relay serves on STATUS: its apply epoch (empty if none).
status_recovery_epoch() {
  fetch_beacon_state 2>/dev/null | sed -n 's/^Recovery: pending, applies at epoch \([0-9]*\) .*$/\1/p'
}

run_recovery_scenario() {
  echo -e "${BLUE}═══ L-SMOKE-RECOVERY ═══${NC}"
  # The admin keys are presumed lost: the recovery set announces three new ones (3-of-3, like the
  # genesis set the suite signs with).
  local key_dir="${TEST_DIR}/keys" announcement="${TEST_DIR}/recovery.tx" admin_args=() i reply epoch
  for i in 1 2 3; do
    rm -f "$key_dir/admin-next${i}.key" "$key_dir/admin-next${i}.pub"
    "$BUILD_DIR/app/pp-client" keygen -o "$key_dir/admin-next${i}" >/dev/null || die "pp-client keygen failed"
    admin_args+=(--admin-pubkey "$key_dir/admin-next${i}.pub")
  done
  rm -f "$announcement"
  run_pp_client mk-recovery --network-id "$SMOKE_NETWORK_ID" "${admin_args[@]}" --min-signatures 3 \
    -o "$announcement" >/dev/null || die "mk-recovery failed"
  run_pp_client sign-tx -k "$key_dir/recovery1.key" --network-id "$SMOKE_NETWORK_ID" "$announcement" >/dev/null ||
    die "sign-tx (recovery key) failed"
  run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" submit-tx "$announcement" >/dev/null ||
    die "L-SMOKE-RECOVERY: submit-tx failed"
  echo -e "${CYAN}Submitted the recovery announcement${NC}"

  local deadline=$((SECONDS + 90))
  while ((SECONDS < deadline)); do
    epoch=$(status_recovery_epoch)
    [[ -n "$epoch" ]] && break
    sleep 1
  done
  [[ -n "$epoch" ]] || die "L-SMOKE-RECOVERY: the relay never served a pending recovery"
  grep -a -q "RECOVERY ANNOUNCED" "${TEST_DIR}/beacon/beacon.log" || die "L-SMOKE-RECOVERY: the beacon did not alert"
  echo -e "${GREEN}✓ Recovery pending, applies at epoch ${epoch}${NC}"

  # Frozen: the old admins' certificate is refused, by the relay and the beacon.
  local cert
  cert=$(make_beacon_cert 3 "${TEST_DIR}/beacon/keys/beacon-signing.pub")
  if reply=$(run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" submit-tx "$cert" 2>&1); then
    die "L-SMOKE-RECOVERY: a certificate was taken while the admin set is frozen: ${reply}"
  fi
  [[ -z "$(status_certificate)" || "$(status_certificate)" != 3* ]] ||
    die "L-SMOKE-RECOVERY: the relay holds a certificate the frozen admin set signed"
  echo -e "${GREEN}✓ The frozen admin set's certificate is refused${NC}"

  # Applied at its epoch: STATUS no longer shows it. From here the new keys are the admin set the
  # rest of the suite signs with.
  deadline=$((SECONDS + 180))
  while ((SECONDS < deadline)); do
    [[ -z "$(status_recovery_epoch)" ]] && break
    sleep 1
  done
  [[ -z "$(status_recovery_epoch)" ]] || die "L-SMOKE-RECOVERY: the recovery never applied"
  grep -a -q "RECOVERY APPLIED" "${TEST_DIR}/beacon/beacon.log" || die "L-SMOKE-RECOVERY: the beacon did not apply it"
  for i in 1 2 3; do
    cp "$key_dir/admin-next${i}.key" "$key_dir/genesis${i}.key"
    cp "$key_dir/admin-next${i}.pub" "$key_dir/genesis${i}.pub"
  done
  echo -e "${GREEN}✓ L-SMOKE-RECOVERY: the new admin set is in force${NC}"
}

# --- L-SMOKE-REWIND (beacon-authority ba5c, bp1): a certificate not on chain (a stolen beacon key)
# orders every node holding blocks past its cutover to rewind; each restarts itself ---

# The certificate the relay serves on STATUS: "<generation> <cutover>" (empty if none).
status_certificate() {
  fetch_beacon_state 2>/dev/null | sed -n 's/^Certificate: generation \([0-9]*\), cutover \([0-9a-z]*\)$/\1 \2/p'
}

# The certificate relay $1 serves on STATUS, as "generation cutover" (empty if none).
relay_certificate() {
  run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr "$1")" status 2>/dev/null |
    sed -n 's/^Certificate: generation \([0-9]*\), cutover \([0-9a-z]*\)$/\1 \2/p'
}

# The height of the signed tip the relay serves (empty if none).
status_signed_tip_height() {
  fetch_beacon_state 2>/dev/null | sed -n 's/^Signed tip: block \([0-9]*\), generation [0-9]*$/\1/p'
}

# The rewind the relay reports on STATUS: "<keptUpTo> <generation>" (empty if none).
status_rewound() {
  fetch_beacon_state 2>/dev/null |
    sed -n 's/^Rewound: kept blocks up to \([0-9]*\), dropped [0-9]* to [0-9]* (beacon generation \([0-9]*\) cutover)$/\1 \2/p'
}

# Every process in the smoke network is still running (a rewind restarts in-process).
all_processes_alive() {
  local name pid
  while IFS=: read -r name pid; do
    kill -0 "$pid" 2>/dev/null || { echo "$name"; return 1; }
  done <"$PID_FILE"
}

run_out_of_band_scenario() {
  echo -e "${BLUE}═══ L-SMOKE-REWIND (out of band) ═══${NC}"
  # The admins learn the beacon key was stolen two blocks ago: they certify generation 3 for a key
  # no running beacon holds, trusting the old generation only up to then, and hand the
  # certificate to the relay. It is not on chain, and the old beacon will never commit it. It names
  # relay2 — a second relay, below the same beacon — as where the new beacon is reached (bp2b).
  start_relay relay2 "$RELAY2_PORT"
  local hint
  hint=$(relay_multiaddr relay2)
  local tip cutover cert reply key="${TEST_DIR}/unheld-beacon-signing"
  tip=$(status_signed_tip_height)
  [[ -n "$tip" && "$tip" -ge 2 ]] || die "L-SMOKE-REWIND: no signed tip to start from (got '${tip}')"
  cutover=$((tip - 2))
  # The last miner is down while the cut happens; it comes back afterwards (below). It goes down
  # holding a block past the cutover (it may trail the relay), or it would have nothing to rewind.
  local offline=$NUM_MINERS
  wait_for_miner_tip "$offline" "$tip" 60 || die "L-SMOKE-REWIND: miner${offline} did not reach block ${tip}"
  stop_miner_by_id "$offline"
  rm -f "$key.key" "$key.pub"
  "$BUILD_DIR/app/pp-client" keygen -o "$key" >/dev/null || die "pp-client keygen failed"
  cert=$(make_beacon_cert 3 "$key.pub" "$cutover" "$hint")
  reply=$(run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr)" submit-tx "$cert" 2>&1) ||
    die "L-SMOKE-REWIND: submit-tx failed: ${reply}"
  echo -e "${CYAN}Handed the relay a generation-3 certificate (cutover ${cutover}; signed tip was ${tip})${NC}"

  # Every node holding blocks past the cutover rewinds to it by restarting itself, unattended.
  local deadline=$((SECONDS + 60)) rewound=""
  while ((SECONDS < deadline)); do
    rewound=$(status_rewound)
    [[ "$rewound" == "${cutover} 3" ]] && break
    sleep 1
  done
  [[ "$rewound" == "${cutover} 3" ]] ||
    die "L-SMOKE-REWIND: the relay did not report a rewind to block ${cutover} (got '${rewound}')"
  [[ "$(get_next_block_id)" == "$((cutover + 1))" ]] ||
    die "L-SMOKE-REWIND: the relay holds blocks past the cutover (nextBlockId $(get_next_block_id))"
  [[ "$(status_certificate)" == "3 ${cutover}" ]] ||
    die "L-SMOKE-REWIND: the relay does not serve generation 3 after its restart (got '$(status_certificate)')"
  local served
  served=$(status_signed_tip_height)
  [[ -z "$served" || "$served" -le "$cutover" ]] ||
    die "L-SMOKE-REWIND: the relay serves a tip past the cutover (block ${served})"
  echo -e "${GREEN}✓ The relay rewound to block ${cutover}, restarted, and serves generation 3${NC}"

  # The relay forwarded it: the beacon holds no key it names, so it stands down; it rewinds too.
  grep -a -q "Beacon generation 3 (seen out of band) certifies none of this beacon's keys" \
    "${TEST_DIR}/beacon/beacon.log" || die "L-SMOKE-REWIND: the beacon did not stand down"
  deadline=$((SECONDS + 30))
  until grep -a -q "Rewound: kept blocks up to ${cutover}," "${TEST_DIR}/beacon/beacon.log"; do
    ((SECONDS < deadline)) || die "L-SMOKE-REWIND: the beacon did not rewind"
    sleep 1
  done
  # Miners learn it from the relay's STATUS once it is back, then rewind and restart themselves;
  # their restart comes up although the beacon that stood down refuses their registration.
  local i
  for i in $(seq 1 $((offline - 1))); do
    deadline=$((SECONDS + 60))
    until grep -a -q "Rewound: kept blocks up to ${cutover}," "${TEST_DIR}/miner${i}/miner.log"; do
      ((SECONDS < deadline)) || die "L-SMOKE-REWIND: miner${i} did not rewind"
      sleep 1
    done
  done
  local miners_rewound=$((offline - 1))
  local stopped
  stopped=$(all_processes_alive) || die "L-SMOKE-REWIND: ${stopped} stopped instead of restarting"
  echo -e "${GREEN}✓ The beacon and ${miners_rewound} miner(s) rewound too; every process restarted in place${NC}"

  # The miner that was down still holds blocks past the cutover: back online, it learns the
  # certificate from the relay's STATUS in its startup sync and rewinds on its own.
  start_miner "$offline" || die "L-SMOKE-REWIND: miner${offline} did not come back"
  deadline=$((SECONDS + 60))
  until grep -a -q "Rewound: kept blocks up to ${cutover}," "${TEST_DIR}/miner${offline}/miner.log"; do
    ((SECONDS < deadline)) || die "L-SMOKE-REWIND: miner${offline}, back online, did not rewind"
    sleep 1
  done
  stopped=$(all_processes_alive) || die "L-SMOKE-REWIND: ${stopped} stopped instead of restarting"
  echo -e "${GREEN}✓ Miner${offline}, down during the cut, rewound once back online${NC}"
  check_dial_hint_adopted "$hint" "$cutover" "$offline"

  local held
  held=$(get_next_block_id)
  inject_transactions_for_block_production >/dev/null 2>&1 || true
  sleep $((SLOT_DURATION * 4))
  [[ "$(get_next_block_id)" == "$held" ]] || die "L-SMOKE-REWIND: the relay took blocks past the cutover"
  echo -e "${GREEN}✓ L-SMOKE-REWIND: every node rewound to the cutover on its own; nothing commits past it${NC}"
}

# bp2b: the certificate's dial hint ($1, relay2) became an upstream of relay1 and of miners 1..$3;
# relay2 holds the certificate (cutover $2) — pushed up by relay1, or from the beacon's STATUS —
# and did not take its own address for an upstream.
check_dial_hint_adopted() {
  local hint=$1 cutover=$2 miners=$3 i log deadline
  local adopted="Added upstream [1] ${hint} (a beacon certificate's hint)"
  for log in "${TEST_DIR}/relay/relay.log" $(for i in $(seq 1 "$miners"); do echo "${TEST_DIR}/miner${i}/miner.log"; done); do
    deadline=$((SECONDS + 30))
    until grep -a -F -q "$adopted" "$log"; do
      ((SECONDS < deadline)) || die "L-SMOKE-REWIND: $(basename "$(dirname "$log")") did not add the dial hint as an upstream"
      sleep 1
    done
  done
  [[ "$(relay_certificate relay2)" == "3 ${cutover}" ]] ||
    die "L-SMOKE-REWIND: relay2 does not hold generation 3 (got '$(relay_certificate relay2)')"
  ! grep -a -q "Added upstream" "${TEST_DIR}/relay2/relay.log" ||
    die "L-SMOKE-REWIND: relay2 took a dial hint for an upstream — its own address"
  echo -e "${GREEN}✓ Relay1 and ${miners} miner(s) took the certificate's dial hint (relay2) as an upstream; relay2 holds it${NC}"
}

# Restart miner $1 with no data — "anchored" pins genesis — and check it starts at a checkpoint at
# or above $3, catches up, and keeps up past the next checkpoint: from there it checks every block
# in Full (stateRoot included), which proves its warm-up exact. (It does not seal yet: its history
# is shorter than the transaction validity window, as for any join on a chain this young.)
join_fresh_miner() {
  local miner_id=$1 mode=$2 above=$3
  local log="${TEST_DIR}/miner${miner_id}/miner.log"
  stop_miner_by_id "$miner_id"
  rm -rf "${TEST_DIR}/miner${miner_id}/data"
  local log_lines
  log_lines=$(wc -l <"$log" 2>/dev/null || echo 0)
  if [[ "$mode" == "anchored" ]]; then
    start_anchored_miner "$miner_id" || die "miner${miner_id} (anchored) did not start"
  else
    start_miner "$miner_id" || die "miner${miner_id} did not start"
  fi
  local started
  started=$(tail -n +$((log_lines + 1)) "$log" |
    sed -n 's/.*No local blocks: authenticating blocks \([0-9]*\) to .*/\1/p' | head -1)
  [[ -n "$started" && "$started" -ge "$above" ]] ||
    die "miner${miner_id} (${mode}) did not start at a checkpoint >= ${above} (started at '${started}')"
  if [[ "$mode" == "anchored" ]]; then
    tail -n +$((log_lines + 1)) "$log" | grep -a -q "authority proof ties it to the anchored genesis" ||
      die "miner${miner_id} did not tie its checkpoint to the anchored genesis"
  fi
  wait_for_miner_tip "$miner_id" "$(get_next_block_id)" 90 || die "miner${miner_id} (${mode}) did not catch up"
  # A later checkpoint than the one after its start: it validated Full blocks in between.
  local later
  later=$(wait_for_checkpoint_above "$started" 180) ||
    die "miner${miner_id} (${mode}): the chain did not pass the checkpoint after ${started}"
  wait_for_miner_tip "$miner_id" "$(get_next_block_id)" 90 ||
    die "miner${miner_id} (${mode}) fell behind after its warm-up (Full validation failed?)"
  if tail -n +$((log_lines + 1)) "$log" | grep -a -q "Block sync failed"; then
    tail -n +$((log_lines + 1)) "$log" | grep -a "Block sync failed" | tail -3 >&2
    die "miner${miner_id} (${mode}) had sync failures after joining"
  fi
  echo -e "${GREEN}✓ Miner${miner_id} (${mode}) started at checkpoint ${started} and validated Full past checkpoint ${later}${NC}"
}

# The checkpointId the relay reports.
get_checkpoint_id() {
  fetch_beacon_state 2>/dev/null | sed -n 's/.*"checkpointId": *\([0-9]*\).*/\1/p' | head -1
}

# Wait (keeping blocks coming) until the reported checkpoint is above block $1; prints it.
wait_for_checkpoint_above() {
  local above=$1 max_wait=${2:-120} deadline checkpoint
  deadline=$((SECONDS + max_wait))
  while ((SECONDS < deadline)); do
    checkpoint=$(get_checkpoint_id)
    if [[ -n "$checkpoint" && "$checkpoint" -gt "$above" ]]; then
      echo "$checkpoint"
      return 0
    fi
    inject_transactions_for_block_production >/dev/null 2>&1 || true
    sleep 2
  done
  return 1
}

# Hex of the genesis block hash, as node $1 (default: the relay) serves block 0.
genesis_hash_hex() {
  local host=${1:-$(relay_multiaddr)}
  run_pp_client ${DEBUG_FLAG} -b --host "$host" block 0 2>/dev/null |
    sed -n 's/.*"hash": *"\(0x[0-9a-fA-F]*\)".*/\1/p' | head -1
}

# Start a miner whose config pins genesis (networkAnchor.genesisHash).
start_anchored_miner() {
  local miner_id=$1
  local miner_dir="${TEST_DIR}/miner${miner_id}"
  local miner_port=$((MINER_BASE_PORT + miner_id - 1)) hash
  hash=$(genesis_hash_hex)
  [[ -n "$hash" ]] || die "could not read the genesis hash"
  mkdir -p "$miner_dir"
  [[ -f "$miner_dir/.signature" ]] || touch "$miner_dir/.signature"
  create_miner_config "$miner_id" "$miner_dir" "$miner_port"
  sed -i "s|^  \"beacons\"|  \"networkAnchor\": {\"genesisHash\": \"${hash}\"},\n  \"beacons\"|" "$miner_dir/config.json"
  grep -q '"networkAnchor"' "$miner_dir/config.json" || die "failed to pin genesis in miner${miner_id} config"
  run_bg_cmd "${miner_dir}/console.log" "$BUILD_DIR/app/pp-miner" -d "$miner_dir" ${DEBUG_FLAG}
  save_pid "miner${miner_id}" $!
  await_miner "$miner_id"
}


# The signed tip relay $1 serves on STATUS, as "height generation" (empty if none).
relay_signed_tip() {
  run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr "$1")" status 2>/dev/null |
    sed -n 's/^Signed tip: block \([0-9]*\), generation \([0-9]*\)$/\1 \2/p'
}

# The log file of node $1: its role's log (a promoted relay logs to beacon.log from then on).
node_log() {
  case "$1" in
    beacon) echo "${TEST_DIR}/beacon/beacon.log" ;;
    miner*) echo "${TEST_DIR}/$1/miner.log" ;;
    *) echo "${TEST_DIR}/$1/relay.log" ;;
  esac
}

# Wait up to $3 s until relay $1's nextBlockId reaches $2.
wait_for_relay_tip() {
  local name=$1 target=$2 max_wait=${3:-60} deadline got
  deadline=$((SECONDS + max_wait))
  while ((SECONDS < deadline)); do
    got=$(parse_next_block_id "$(run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr "$name")" status 2>&1)")
    [[ -n "$got" && "$got" -ge "$target" ]] && return 0
    sleep 0.5
  done
  return 1
}

# Wait up to $3 s until log $1 holds the fixed string $2.
wait_for_log() {
  local log=$1 text=$2 max_wait=${3:-60} deadline
  deadline=$((SECONDS + max_wait))
  until grep -a -F -q "$text" "$log" 2>/dev/null; do
    ((SECONDS < deadline)) || return 1
    sleep 1
  done
}

# Case 2 end to end (beacon-authority bp3): the beacon key is stolen. The admins certify generation
# 2 for a key the maintainer generated on relay2, trusting generation 1 only up to two blocks back,
# with relay3 — a relay in front of relay2 — as the way to reach it. Every node rewinds; the
# maintainer promotes relay2 (pp-beacon --promote); the next leader commits the certificate in the
# block after the cutover, and the network resumes under generation 2 — relay1 and the miners,
# configured for the old beacon's side only, find it through the hint.
run_takeover_scenario() {
  echo -e "${BLUE}═══ L-SMOKE-TAKEOVER (case 2) ═══${NC}"
  wait_for_blocks 4 90 || die "L-SMOKE-TAKEOVER: the network did not get going"
  local relay2="${TEST_DIR}/relay2" i
  rm -f "$relay2/keys/beacon-signing.key" "$relay2/keys/beacon-signing.pub"
  "$BUILD_DIR/app/pp-client" keygen -o "$relay2/keys/beacon-signing" >/dev/null || die "pp-client keygen failed"
  local tip cutover cert reply
  tip=$(status_signed_tip_height)
  [[ -n "$tip" && "$tip" -ge 2 ]] || die "L-SMOKE-TAKEOVER: no signed tip to start from (got '${tip}')"
  cutover=$((tip - 2))
  # Every node holds blocks past the cutover first (some trail the relay), so every node rewinds.
  for i in relay2 relay3; do
    wait_for_relay_tip "$i" "$tip" 60 || die "L-SMOKE-TAKEOVER: ${i} did not reach block ${tip}"
  done
  for i in $(seq 1 "$NUM_MINERS"); do
    wait_for_miner_tip "$i" "$tip" 60 || die "L-SMOKE-TAKEOVER: miner${i} did not reach block ${tip}"
  done
  cert=$(make_beacon_cert 2 "$relay2/keys/beacon-signing.pub" "$cutover" "$(relay_multiaddr relay3)")
  for i in relay relay2; do # the maintainer's relays
    reply=$(run_pp_client ${DEBUG_FLAG} -b --host "$(relay_multiaddr "$i")" submit-tx "$cert" 2>&1) ||
      die "L-SMOKE-TAKEOVER: submit-tx to ${i} failed: ${reply}"
  done
  echo -e "${CYAN}Certified generation 2 for relay2's key (cutover ${cutover}; signed tip was ${tip}), hint relay3${NC}"

  # Everyone rewinds to the cutover; the old beacon, holding no key generation 2 names, stands down.
  local node log
  for node in beacon relay relay2 relay3 $(seq -f "miner%g" 1 "$NUM_MINERS"); do
    log=$(node_log "$node")
    wait_for_log "$log" "Rewound: kept blocks up to ${cutover}," 90 || die "L-SMOKE-TAKEOVER: ${node} did not rewind"
  done
  grep -a -q "certifies none of this beacon's keys" "${TEST_DIR}/beacon/beacon.log" ||
    die "L-SMOKE-TAKEOVER: the old beacon did not stand down"
  echo -e "${GREEN}✓ Every node rewound to block ${cutover}; the old beacon stood down${NC}"

  # The maintainer promotes relay2: same directory, same identity and port, now the beacon.
  stop_node relay2
  run_bg_cmd "$relay2/console.log" "$BUILD_DIR/app/pp-beacon" -d "$relay2" --promote ${DEBUG_FLAG}
  save_pid "relay2" $!
  wait_until_ready relay2 "$relay2/console.log" 90 || die "L-SMOKE-TAKEOVER: relay2 did not start as the beacon"
  grep -a -q "Promoted: this relay's work directory now runs the beacon" "$relay2/beacon.log" ||
    die "L-SMOKE-TAKEOVER: relay2 was not promoted"
  [[ -f "$relay2/config.relay.json" ]] || die "L-SMOKE-TAKEOVER: the relay's config was not kept"
  grep -a -q "Clock check: " "$relay2/beacon.log" || die "L-SMOKE-TAKEOVER: no clock check"
  echo -e "${GREEN}✓ Relay2 promoted: taking over as beacon generation 2${NC}"

  # The next leader commits the certificate in block cutover+1; the new beacon signs it.
  local deadline=$((SECONDS + 120)) committed=""
  while ((SECONDS < deadline)); do
    committed=$(grep -a -l "commits the beacon certificate held" "${TEST_DIR}"/miner*/miner.log 2>/dev/null | head -1)
    [[ -n "$committed" ]] && grep -a -q "Beacon certificate generation 2 in force from block $((cutover + 1))" \
      "$relay2/beacon.log" && break
    sleep 1
  done
  grep -a -q "Beacon certificate generation 2 in force from block $((cutover + 1))" "$relay2/beacon.log" ||
    die "L-SMOKE-TAKEOVER: the new beacon did not commit the certificate in block $((cutover + 1))"
  echo -e "${GREEN}✓ $(basename "$(dirname "$committed")") led block $((cutover + 1)) with the certificate; the new beacon committed it${NC}"

  # Relay1 and the miners follow generation 2 through relay3, the hint; production resumes.
  inject_transactions_for_block_production >/dev/null 2>&1 || true
  local served="" target=$((cutover + 4))
  deadline=$((SECONDS + 180))
  while ((SECONDS < deadline)); do
    served=$(relay_signed_tip relay)
    [[ -n "$served" && "${served#* }" == "2" && "${served% *}" -ge "$target" ]] && break
    sleep 2
  done
  [[ -n "$served" && "${served#* }" == "2" && "${served% *}" -ge "$target" ]] ||
    die "L-SMOKE-TAKEOVER: relay1 does not serve generation 2 up to block ${target} (serves '${served}')"
  grep -a -F -q "Following upstream [1] $(relay_multiaddr relay3)" "${TEST_DIR}/relay/relay.log" ||
    die "L-SMOKE-TAKEOVER: relay1 did not move to relay3"
  for i in $(seq 1 "$NUM_MINERS"); do
    wait_for_miner_tip "$i" "$target" 90 || die "L-SMOKE-TAKEOVER: miner${i} did not follow generation 2"
    wait_for_log "${TEST_DIR}/miner${i}/miner.log" "Clock recalibrated for beacon generation 2" 60 ||
      die "L-SMOKE-TAKEOVER: miner${i} did not recalibrate for generation 2"
  done
  local stopped
  stopped=$(all_processes_alive) || die "L-SMOKE-TAKEOVER: ${stopped} stopped"
  echo -e "${GREEN}✓ L-SMOKE-TAKEOVER: relay1 and ${NUM_MINERS} miner(s) follow generation 2 (block ${served% *}) through relay3; clocks recalibrated${NC}"
}
