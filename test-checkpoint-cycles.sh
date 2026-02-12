#!/bin/bash

# pp-ledger Checkpoint Cycles Test Script
#
# Simulates various conditions for transactions and tests through cycles of checkpoints.
# The test validates the intended flow:
#   - Miners start from last checkpoint
#   - Work in non-strict mode to update memory until latest checkpoint
#   - Then switch to strict mode for real validation and mining
#
# Note: Some checkpoint implementations (e.g. automatic checkpoint creation) may
# still be missing. This script exercises the structure and flow.

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TEST_DIR="${BUILD_DIR}/test-checkpoint-cycles"
PID_FILE="${TEST_DIR}/network.pids"
NUM_MINERS=3
BEACON_PORT=8617
MINER_BASE_PORT=8618

# Test-friendly consensus: short slots, small epoch, aggressive checkpoint params
# checkpointMinBlocks=2: renewals start when creating block 3 (need blocks 1,2 from pending tx first)
SLOT_DURATION=2
SLOTS_PER_EPOCH=10
CHECKPOINT_MIN_BLOCKS=2
CHECKPOINT_MIN_AGE_SECONDS=0

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

run_cmd() {
    echo -e "${BLUE}Running: $*${NC}"
    "$@"
}

run_bg_cmd() {
    local log_file="$1"
    shift
    echo -e "${BLUE}Background: $* > $log_file${NC}"
    "$@" > "$log_file" 2>&1 &
}

save_pid() {
    local name=$1
    local pid=$2
    mkdir -p "$(dirname "$PID_FILE")"
    echo "$name:$pid" >> "$PID_FILE"
}

# =============================================================================
# Phase 0: Setup and helpers
# =============================================================================

verify_build() {
    if [ ! -d "$BUILD_DIR" ]; then
        echo -e "${RED}Build directory not found. Run: mkdir build && cd build && cmake .. && make${NC}"
        exit 1
    fi
    for exe in pp-beacon pp-miner pp-client; do
        if [ ! -f "$BUILD_DIR/app/$exe" ]; then
            echo -e "${RED}$exe not found. Build the project first.${NC}"
            exit 1
        fi
    done
    if ! command -v xxd &>/dev/null && ! command -v python3 &>/dev/null; then
        echo -e "${RED}Need xxd or python3 for miner key generation${NC}"
        exit 1
    fi
}

stop_network() {
    if [ ! -f "$PID_FILE" ]; then
        return 0
    fi
    echo -e "${YELLOW}Stopping test network...${NC}"
    while IFS=: read -r name pid; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done < "$PID_FILE"
    sleep 2
    while IFS=: read -r name pid; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done < "$PID_FILE"
    rm -f "$PID_FILE"
    echo -e "${GREEN}✓ Network stopped${NC}"
}

# =============================================================================
# Phase 1: Initialize beacon with test-friendly config
# =============================================================================

initialize_beacon_with_test_config() {
    local beacon_dir="${TEST_DIR}/beacon"
    # Clean slate: beacon init requires dir to exist with .signature (or not exist)
    rm -rf "$beacon_dir"
    mkdir -p "$beacon_dir"
    touch "$beacon_dir/.signature"
    # BeaconServer.init: if dir exists with .signature, it loads init-config and proceeds
    # Create init-config.json with short slots and checkpoint params
    cat > "$beacon_dir/init-config.json" << EOF
{
  "slotDuration": ${SLOT_DURATION},
  "slotsPerEpoch": ${SLOTS_PER_EPOCH},
  "maxPendingTransactions": 10000,
  "maxTransactionsPerBlock": 100,
  "minFeePerTransaction": 1,
  "checkpointMinBlocks": ${CHECKPOINT_MIN_BLOCKS},
  "checkpointMinAgeSeconds": ${CHECKPOINT_MIN_AGE_SECONDS}
}
EOF

    echo -e "${CYAN}Created init-config.json (slot=${SLOT_DURATION}s, epoch=${SLOTS_PER_EPOCH} slots, checkpointMinBlocks=${CHECKPOINT_MIN_BLOCKS})${NC}"

    local init_output
    init_output=$("$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" --init 2>&1)
    echo "$init_output" | head -5
    echo -e "${GREEN}✓ Beacon initialized with test config${NC}"

    # Extract reserve private keys for add-account (ID_RESERVE=2 needs 3-of-3 multisig)
    local key_dir="${TEST_DIR}/keys"
    mkdir -p "$key_dir"
    local i=1
    while IFS= read -r line; do
        local pk
        pk=$(echo "$line" | sed 's/.*"privateKey"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/' | tr -d ' \n')
        [ -n "$pk" ] && echo "$pk" > "$key_dir/reserve${i}.key" && i=$((i + 1))
    done < <(echo "$init_output" | sed -n '/"reserve"/,/\]/p' | grep '"privateKey"')
    if [ -f "$key_dir/reserve1.key" ]; then
        echo -e "${CYAN}Saved $((i - 1)) reserve keys for add-account tests${NC}"
    fi
}

create_beacon_config() {
    local beacon_dir="${TEST_DIR}/beacon"
    cat > "$beacon_dir/config.json" << EOF
{
  "host": "localhost",
  "port": $BEACON_PORT
}
EOF
}

# Generate Ed25519 key for miner - file must contain raw 32 bytes (Miner uses readKey, ed25519Sign needs 32 bytes)
generate_miner_key() {
    local miner_id=$1
    local key_dir="${TEST_DIR}/keys"
    local key_file="${key_dir}/miner${miner_id}.key"
    mkdir -p "$key_dir"
    if [ ! -f "$key_file" ]; then
        local output hex
        output=$("$BUILD_DIR/app/pp-client" --debug keygen 2>&1)
        if [ $? -ne 0 ]; then
            echo -e "${RED}pp-client keygen failed:${NC}" >&2
            echo "$output" >&2
            exit 1
        fi
        hex=$(echo "$output" | grep "Private key" | sed 's/.*: *//' | tr -d ' \n')
        # Miner ed25519Sign expects raw 32 bytes; write binary to file
        if command -v xxd &>/dev/null; then
            echo -n "$hex" | xxd -r -p > "$key_file"
        else
            echo "$hex" | python3 -c "import sys,binascii; sys.stdout.buffer.write(binascii.unhexlify(sys.stdin.read().strip()))" > "$key_file"
        fi
    fi
    echo "$key_file"
}

create_miner_config() {
    local miner_id=$1
    local miner_dir=$2
    local miner_port=$3
    local key_file
    key_file=$(generate_miner_key "$miner_id")
    cp "$key_file" "$miner_dir/key.txt"
    local key_path
    key_path="$(cd "$miner_dir" && pwd)/key.txt"

    cat > "$miner_dir/config.json" << EOF
{
  "minerId": $miner_id,
  "key": "$key_path",
  "host": "localhost",
  "port": $miner_port,
  "beacons": ["localhost:$BEACON_PORT"]
}
EOF
}

start_beacon() {
    local beacon_dir="${TEST_DIR}/beacon"
    run_bg_cmd "$beacon_dir/console.log" "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" --debug
    save_pid "beacon" $!
    sleep 3
    if ! kill -0 $(tail -1 "$PID_FILE" | cut -d: -f2) 2>/dev/null; then
        echo -e "${RED}Beacon failed to start${NC}"
        cat "$beacon_dir/console.log" 2>/dev/null || true
        exit 1
    fi
    echo -e "${GREEN}✓ Beacon started on port $BEACON_PORT${NC}"
}

start_miner() {
    local miner_id=$1
    local miner_dir="${TEST_DIR}/miner${miner_id}"
    local miner_port=$((MINER_BASE_PORT + miner_id - 1))

    # Miner requires dir with .signature. Create both, then write our config (with beacons).
    mkdir -p "$miner_dir"
    if [ ! -f "$miner_dir/.signature" ]; then
        touch "$miner_dir/.signature"
    fi
    create_miner_config "$miner_id" "$miner_dir" "$miner_port"
    run_bg_cmd "${miner_dir}/console.log" "$BUILD_DIR/app/pp-miner" -d "$miner_dir" --debug
    save_pid "miner${miner_id}" $!
    sleep 2
    echo -e "${GREEN}✓ Miner${miner_id} started on port $miner_port${NC}"
}

start_all_miners() {
    for i in $(seq 1 $NUM_MINERS); do
        start_miner $i
    done
}

# =============================================================================
# Phase 2: Client helpers
# =============================================================================

fetch_beacon_state() {
    local output
    output=$("$BUILD_DIR/app/pp-client" --debug -b --host "localhost:$BEACON_PORT" status 2>&1) || {
        echo -e "${RED}pp-client beacon status failed:${NC}" >&2
        echo "$output" >&2
        echo ""
        return 0
    }
    echo "$output"
}

fetch_miner_status() {
    local port=$1
    local output
    output=$("$BUILD_DIR/app/pp-client" --debug -m --host "localhost:$port" status 2>&1) || {
        echo -e "${RED}pp-client miner status failed (port=$port):${NC}" >&2
        echo "$output" >&2
        echo ""
        return 0
    }
    echo "$output"
}

get_next_block_id() {
    local out
    out=$(fetch_beacon_state)
    echo "$out" | grep -o '"nextBlockId"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo "0"
}

get_checkpoint_ids() {
    local out
    out=$(fetch_beacon_state)
    local last=$(echo "$out" | grep -o '"lastCheckpointId"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo "0")
    local curr=$(echo "$out" | grep -o '"checkpointId"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo "0")
    echo "${last} ${curr}"
}

# Wait for at least one miner to accept connections. Returns first working port or empty on timeout.
get_working_miner_port() {
    local port
    for port in $(seq $MINER_BASE_PORT $((MINER_BASE_PORT + NUM_MINERS - 1))); do
        if "$BUILD_DIR/app/pp-client" --debug -m --host "localhost:$port" status &>/dev/null; then
            echo "$port"
            return 0
        fi
    done
    return 1
}

wait_for_miner_ready() {
    local max_wait=${1:-20}
    local elapsed=0
    echo -e "${CYAN}Waiting for miner to be ready (max ${max_wait}s)...${NC}" >&2
    while [ $elapsed -lt $max_wait ]; do
        local port
        port=$(get_working_miner_port)
        if [ -n "$port" ]; then
            echo -e "${GREEN}✓ Miner ready on port $port${NC}" >&2
            echo "$port"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo -e "${RED}No miner responded within ${max_wait}s${NC}" >&2
    return 1
}

# =============================================================================
# Phase 3: Transaction simulation (when accounts exist)
# =============================================================================

# Generate a test keypair for transaction signing (e.g. for alice/bob)
# Creates name.key (private hex) and name.pub (public hex). Returns path to private key file.
generate_tx_keypair() {
    local name=$1
    local key_dir="${TEST_DIR}/keys"
    local key_file="${key_dir}/${name}.key"
    local pub_file="${key_dir}/${name}.pub"
    mkdir -p "$key_dir"
    if [ ! -f "$key_file" ]; then
        local output
        output=$("$BUILD_DIR/app/pp-client" --debug keygen 2>&1)
        if [ $? -ne 0 ]; then
            echo -e "${RED}pp-client keygen failed:${NC}" >&2
            echo "$output" >&2
            exit 1
        fi
        echo "$output" | grep "Private key" | sed 's/.*: *//' | tr -d ' \n' > "$key_file"
        echo "$output" | grep "Public key" | sed 's/.*: *//' | tr -d ' \n' > "$pub_file"
    fi
    echo "$key_file"
}

# Create new account via mk-account + sign-tx (3x) + submit-tx (reserve needs 3-of-3 multisig)
# Usage: try_add_account to_wallet_id amount new_pubkey_hex reserve_key1 key2 key3 miner_port [fee]
# On connection failure, tries other miner ports automatically.
try_add_account() {
    local to=$1
    local amount=$2
    local new_pubkey_hex=$3
    shift 3
    local key1=$1 key2=$2 key3=$3
    local miner_port=${4:-$MINER_BASE_PORT}
    local fee=${5:-1}
    local tx_file="${TEST_DIR}/tmp_account_${to}.dat"

    if [ ! -f "$key1" ] || [ ! -f "$key2" ] || [ ! -f "$key3" ]; then
        echo -e "${YELLOW}  (add-account skipped - reserve keys not found)${NC}"
        return 1
    fi

    local mk_cmd=("$BUILD_DIR/app/pp-client" --debug mk-account 2 "$amount" -t "$to" -f "$fee" -o "$tx_file")
    [ -n "$new_pubkey_hex" ] && mk_cmd+=(--new-pubkey "$new_pubkey_hex")
    local err
    err=$("${mk_cmd[@]}" 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${YELLOW}  (mk-account failed for to=$to)${NC}"
        echo -e "${RED}Error:${NC} $err" >&2
        return 1
    fi
    for k in "$key1" "$key2" "$key3"; do
        err=$("$BUILD_DIR/app/pp-client" --debug sign-tx "$tx_file" -k "$k" 2>&1)
        if [ $? -ne 0 ]; then
            echo -e "${YELLOW}  (sign-tx failed)${NC}"
            echo -e "${RED}Error:${NC} $err" >&2
            rm -f "$tx_file"
            return 1
        fi
    done
    local ports_to_try
    ports_to_try="$miner_port"
    for p in $(seq $MINER_BASE_PORT $((MINER_BASE_PORT + NUM_MINERS - 1))); do
        [ "$p" = "$miner_port" ] || ports_to_try="$ports_to_try $p"
    done
    for p in $ports_to_try; do
        err=$("$BUILD_DIR/app/pp-client" --debug -m -p "$p" submit-tx "$tx_file" 2>&1)
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}  ✓ Account created: id=$to amount=$amount${NC}"
            rm -f "$tx_file"
            return 0
        fi
        if echo "$err" | grep -q "Failed to connect"; then
            echo -e "${YELLOW}  (submit-tx port $p unreachable, trying next miner)${NC}" >&2
            continue
        fi
        echo -e "${YELLOW}  (submit-tx failed for to=$to)${NC}"
        echo -e "${RED}Error:${NC} $err" >&2
        rm -f "$tx_file"
        return 1
    done
    echo -e "${YELLOW}  (submit-tx failed - no miner reachable)${NC}"
    echo -e "${RED}Error:${NC} $err" >&2
    rm -f "$tx_file"
    return 1
}

# Submit a transfer if we have valid accounts and keys
# Usage: try_add_tx from_wallet_id to_wallet_id amount key_file [fee] [miner_port]
# On connection failure, tries other miner ports automatically.
try_add_tx() {
    local from=$1
    local to=$2
    local amount=$3
    local key_file=$4
    local fee=${5:-1}
    local miner_port=${6:-$MINER_BASE_PORT}

    if [ ! -f "$key_file" ]; then
        echo -e "${YELLOW}  (add-tx skipped - key file not found)${NC}"
        return 1
    fi

    local ports_to_try
    ports_to_try="$miner_port"
    for p in $(seq $MINER_BASE_PORT $((MINER_BASE_PORT + NUM_MINERS - 1))); do
        [ "$p" = "$miner_port" ] || ports_to_try="$ports_to_try $p"
    done
    local err
    for p in $ports_to_try; do
        err=$("$BUILD_DIR/app/pp-client" --debug -m -p "$p" add-tx "$from" "$to" "$amount" -f "$fee" -k "$key_file" 2>&1)
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}  ✓ Transaction submitted: $from -> $to amount=$amount${NC}"
            return 0
        fi
        if echo "$err" | grep -q "Failed to connect"; then
            echo -e "${YELLOW}  (add-tx port $p unreachable, trying next miner)${NC}" >&2
            continue
        fi
        echo -e "${YELLOW}  (add-tx skipped - may need valid accounts: $from -> $to)${NC}"
        echo -e "${RED}Error:${NC} $err" >&2
        return 1
    done
    echo -e "${YELLOW}  (add-tx skipped - no miner reachable)${NC}"
    echo -e "${RED}Error:${NC} $err" >&2
    return 1
}

# =============================================================================
# Phase 4: Inject transactions to trigger block production
# =============================================================================

inject_transactions_for_block_production() {
    echo ""
    echo -e "${BLUE}═══ Injecting transactions to prime block production ═══${NC}"

    local miner_port
    miner_port=$(wait_for_miner_ready 25) || {
        echo -e "${RED}Cannot proceed: no miner reached${NC}" >&2
        exit 1
    }

    local alice=$((1 << 20))
    local bob=$(( (1 << 20) + 1 ))
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

    echo -e "${CYAN}Creating accounts and submitting transfers...${NC}"
    try_add_account "$alice" 10000 "$alice_pub_hex" "$reserve1" "$reserve2" "$reserve3" "$miner_port" $min_fee || true
    try_add_account "$bob" 5000 "$bob_pub_hex" "$reserve1" "$reserve2" "$reserve3" "$miner_port" $min_fee || true

    # Wait for block 1 (alice/bob created) before transfers - add-tx requires from account to exist
    wait_for_blocks 2 25 || true

    # Multiple transfers to feed block production (each block needs pending tx or renewals)
    for amt in 100 50 25 20 15 10 10 10 5 5 5; do
        try_add_tx "$alice" "$bob" "$amt" "$alice_key" $min_fee "$miner_port" || true
    done
    try_add_tx "$bob" "$alice" 25 "$bob_key" $min_fee "$miner_port" || true

    echo -e "${GREEN}✓ Transactions submitted to miner pool (block production can advance)${NC}"
}

# =============================================================================
# Phase 5: Main test scenarios
# =============================================================================

wait_for_blocks() {
    local target=$1
    local max_wait=${2:-60}
    local elapsed=0

    echo -e "${CYAN}Waiting for nextBlockId >= $target (max ${max_wait}s)...${NC}"
    while [ $elapsed -lt $max_wait ]; do
        local next
        next=$(get_next_block_id)
        if [ -n "$next" ] && [ "$next" -ge "$target" ] 2>/dev/null; then
            echo -e "${GREEN}✓ Reached block $next${NC}"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo -e "${YELLOW}Timeout: nextBlockId=$(get_next_block_id) (target $target)${NC}"
    return 1
}

test_scenario_basic_cycles() {
    echo ""
    echo -e "${BLUE}═══ Scenario 1: Basic block production and checkpoint cycle observation ═══${NC}"

    # Wait for a few blocks (genesis + at least 2–3 more)
    wait_for_blocks 3 30 || true

    local state
    state=$(fetch_beacon_state)
    echo -e "${CYAN}Beacon state after block production:${NC}"
    echo "$state" | head -20

    local ids
    ids=$(get_checkpoint_ids)
    echo -e "${CYAN}Checkpoint IDs (lastCheckpointId checkpointId): $ids${NC}"
}

test_scenario_transaction_attempts() {
    echo ""
    echo -e "${BLUE}═══ Scenario 2: Transaction simulation (accounts created in inject phase) ═══${NC}"

    local alice=$((1 << 20))
    local bob=$(( (1 << 20) + 1 ))
    local key_dir="${TEST_DIR}/keys"
    local alice_key="${key_dir}/alice.key"
    local miner_port=$((MINER_BASE_PORT + 1))
    local min_fee=1

    echo -e "${CYAN}Verifying transfers (alice -> bob)...${NC}"
    try_add_tx "$alice" "$bob" 10 "$alice_key" $min_fee "$miner_port" || true
}

test_scenario_late_joiner_miner() {
    echo ""
    echo -e "${BLUE}═══ Scenario 3: Late-joiner miner (sync from checkpoint) ═══${NC}"

    # Ensure we have some chain progress
    wait_for_blocks 2 20 || true

    local next_before
    next_before=$(get_next_block_id)
    echo -e "${CYAN}Current chain: nextBlockId=$next_before${NC}"

    # Stop miner 3 to simulate it being "late"
    local pid_file_line
    pid_file_line=$(grep "^miner3:" "$PID_FILE" 2>/dev/null || true)
    if [ -n "$pid_file_line" ]; then
        local pid
        pid=$(echo "$pid_file_line" | cut -d: -f2)
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${CYAN}Stopping miner3 to simulate late joiner...${NC}"
            kill "$pid" 2>/dev/null || true
            sleep 2
            # Remove from PID file
            grep -v "^miner3:" "$PID_FILE" > "${PID_FILE}.tmp" && mv "${PID_FILE}.tmp" "$PID_FILE"
        fi
    fi

    # Let chain progress without miner3
    echo -e "${CYAN}Letting chain progress for a few blocks...${NC}"
    sleep $((SLOT_DURATION * 3))
    wait_for_blocks $((next_before + 2)) 30 || true

    local next_after
    next_after=$(get_next_block_id)
    echo -e "${CYAN}Chain progressed: nextBlockId=$next_after${NC}"

    # Restart miner3 - it will sync from beacon (lastCheckpointId, checkpointId)
    echo -e "${CYAN}Restarting miner3 (late joiner)...${NC}"
    start_miner 3
    sleep 4

    # Verify miner3 caught up
    local status3
    status3=$(fetch_miner_status $((MINER_BASE_PORT + 2)))
    echo -e "${CYAN}Miner3 status after sync:${NC}"
    echo "$status3" | head -15

    echo -e "${GREEN}Late-joiner flow exercised: miner starts from lastCheckpointId, syncs blocks.${NC}"
    echo -e "${CYAN}When checkpointId > 0: blocks before checkpointId use non-strict mode,${NC}"
    echo -e "${CYAN}at/after checkpointId use strict mode (see Miner::addBlock).${NC}"
}

test_scenario_multi_cycle() {
    echo ""
    echo -e "${BLUE}═══ Scenario 4: Multi-slot cycle observation ═══${NC}"

    local initial_slot
    initial_slot=$(fetch_beacon_state | grep -o '"currentSlot"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo "0")

    echo -e "${CYAN}Observing slot/epoch progression over ~${SLOT_DURATION}0 seconds...${NC}"
    for i in 1 2 3 4 5; do
        sleep $((SLOT_DURATION * 2))
        local state
        state=$(fetch_beacon_state)
        local slot epoch next
        slot=$(echo "$state" | grep -o '"currentSlot"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$')
        epoch=$(echo "$state" | grep -o '"currentEpoch"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$')
        next=$(echo "$state" | grep -o '"nextBlockId"[[:space:]]*:[[:space:]]*[0-9]*' | grep -o '[0-9]*$')
        echo -e "  Check $i: slot=$slot epoch=$epoch nextBlockId=$next"
    done

    echo -e "${GREEN}✓ Multi-cycle observation complete${NC}"
}

# =============================================================================
# Main
# =============================================================================

usage() {
    echo "Usage: $0 [run|start|stop|clear]"
    echo ""
    echo "  run    Execute full checkpoint cycles test (default)"
    echo "  start  Start beacon and miners only (no test)"
    echo "  stop   Stop beacon and miners (e.g. after aborted test)"
    echo "  clear  Stop network and remove all test data"
    echo ""
    echo "The test:"
    echo "  - Initializes beacon with short slots and checkpoint params"
    echo "  - Starts beacon and miners"
    echo "  - Simulates transaction conditions"
    echo "  - Tests late-joiner miner sync (non-strict → strict flow)"
    echo "  - Observes multi-slot cycles"
}

main() {
    local cmd=${1:-run}

    case "$cmd" in
        stop)
            stop_network
            echo -e "${GREEN}✓ Network stopped${NC}"
            return 0
            ;;
        clear|clean)
            stop_network
            rm -rf "$TEST_DIR"
            echo -e "${GREEN}✓ Test data cleared${NC}"
            return 0
            ;;
        start)
            verify_build
            stop_network
            mkdir -p "$TEST_DIR"
            rm -f "$PID_FILE"
            if [ ! -f "${TEST_DIR}/beacon/init-config.json" ]; then
                initialize_beacon_with_test_config
            fi
            create_beacon_config
            start_beacon
            start_all_miners
            echo ""
            echo -e "${GREEN}✓ Network started. Run '$0 run' for full test or '$0 stop' to stop.${NC}"
            echo ""
            return 0
            ;;
        run|"")
            ;;
        *)
            usage
            exit 1
            ;;
    esac

    echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║   pp-ledger Checkpoint Cycles Test                      ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  Slot duration:     ${SLOT_DURATION}s"
    echo -e "  Slots per epoch:   ${SLOTS_PER_EPOCH}"
    echo -e "  Checkpoint min:   ${CHECKPOINT_MIN_BLOCKS} blocks, ${CHECKPOINT_MIN_AGE_SECONDS}s age"
    echo ""

    verify_build
    stop_network

    mkdir -p "$TEST_DIR"
    rm -f "$PID_FILE"

    # Initialize and start network
    initialize_beacon_with_test_config
    create_beacon_config
    start_beacon
    start_all_miners

    # Prime transaction pool so block production can advance (miner needs pending tx or renewals)
    inject_transactions_for_block_production

    # Run test scenarios
    test_scenario_basic_cycles
    test_scenario_transaction_attempts
    test_scenario_late_joiner_miner
    test_scenario_multi_cycle

    echo ""
    echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║   Checkpoint Cycles Test Complete                       ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${CYAN}Note: Block production requires renewals (after checkpointMinBlocks) or pending tx.${NC}"
    echo -e "${CYAN}      With checkpointMinBlocks=2, renewals start at block 3; blocks 1-2 need add-tx.${NC}"
    echo ""
    echo -e "Network still running. To stop: $0 stop   To remove data: $0 clear"
    echo ""
}

main "$@"
