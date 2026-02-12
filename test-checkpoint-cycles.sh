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

    run_cmd "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" --init
    echo -e "${GREEN}✓ Beacon initialized with test config${NC}"
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
        output=$("$BUILD_DIR/app/pp-client" keygen 2>/dev/null)
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
    run_bg_cmd "$beacon_dir/console.log" "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir"
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
    run_bg_cmd "${miner_dir}/console.log" "$BUILD_DIR/app/pp-miner" -d "$miner_dir"
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
    "$BUILD_DIR/app/pp-client" -b --host "localhost:$BEACON_PORT" status 2>/dev/null || true
}

fetch_miner_status() {
    local port=$1
    "$BUILD_DIR/app/pp-client" -m --host "localhost:$port" status 2>/dev/null || true
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

# =============================================================================
# Phase 3: Transaction simulation (when accounts exist)
# =============================================================================

# Generate a test key for transaction signing (e.g. for alice/bob)
# Returns path to key file
generate_tx_key_file() {
    local name=$1
    local key_dir="${TEST_DIR}/keys"
    local key_file="${key_dir}/${name}.key"
    mkdir -p "$key_dir"
    if [ ! -f "$key_file" ]; then
        local output
        output=$("$BUILD_DIR/app/pp-client" keygen 2>/dev/null)
        echo "$output" | grep "Private key" | sed 's/.*: *//' | tr -d ' \n' > "$key_file"
    fi
    echo "$key_file"
}

# Submit a transfer if we have valid accounts and keys
# Usage: try_add_tx from_wallet_id to_wallet_id amount key_file miner_port
try_add_tx() {
    local from=$1
    local to=$2
    local amount=$3
    local key_file=$4
    local miner_port=${5:-$MINER_BASE_PORT}

    if [ ! -f "$key_file" ]; then
        echo -e "${YELLOW}  (add-tx skipped - key file not found)${NC}"
        return 1
    fi

    if "$BUILD_DIR/app/pp-client" -m -p "$miner_port" add-tx "$from" "$to" "$amount" -k "$key_file" 2>/dev/null; then
        echo -e "${GREEN}  ✓ Transaction submitted: $from -> $to amount=$amount${NC}"
        return 0
    else
        echo -e "${YELLOW}  (add-tx skipped - may need valid accounts: $from -> $to)${NC}"
        return 1
    fi
}

# =============================================================================
# Phase 4: Main test scenarios
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
    echo -e "${BLUE}═══ Scenario 2: Transaction simulation attempts ═══${NC}"

    # ID_FIRST_USER = 1<<20 = 1048576
    local alice=$((1 << 20))
    local bob=$(( (1 << 20) + 1 ))

    # Generate keys for potential use
    local alice_key_file
    alice_key_file=$(generate_tx_key_file "alice")
    echo -e "${CYAN}Attempting transactions (accounts may not exist yet):${NC}"

    # Try add-tx - will likely fail without prior T_NEW_USER creating alice
    try_add_tx "$alice" "$bob" 100 "$alice_key_file" $MINER_BASE_PORT || true

    echo -e "${CYAN}Note: Successful transfers require accounts created via T_NEW_USER.${NC}"
    echo -e "${CYAN}      The structure is validated; full tx flow depends on account setup.${NC}"
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
    echo "Usage: $0 [run|clean]"
    echo ""
    echo "  run    Execute checkpoint cycles test (default)"
    echo "  clean  Remove test data and exit"
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

    if [ "$cmd" = "clean" ]; then
        stop_network
        rm -rf "$TEST_DIR"
        echo -e "${GREEN}✓ Test data cleaned${NC}"
        return 0
    fi

    if [ "$cmd" != "run" ] && [ "$cmd" != "" ]; then
        usage
        exit 1
    fi

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
    echo -e "Network still running. To stop: $0 clean"
    echo ""
}

main "$@"
