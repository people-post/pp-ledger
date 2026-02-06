#!/bin/bash

# pp-ledger Test Network Control Script
# This script manages a test network with 1 beacon and multiple miners

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TEST_DIR="${BUILD_DIR}/test-network"
PID_FILE="${TEST_DIR}/network.pids"
NUM_MINERS=3
BEACON_PORT=8517
MINER_BASE_PORT=8518
DEBUG_MODE=false

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Run and print command
run_cmd() {
    echo -e "${BLUE}Running: $@${NC}"
    "$@"
}

# Run and print command in background with output redirection
run_bg_cmd() {
    local log_file="$1"
    shift
    echo -e "${BLUE}Running: $@ > $log_file 2>&1 &${NC}"
    "$@" > "$log_file" 2>&1 &
}

# Print usage
usage() {
    echo "Usage: $0 COMMAND [OPTIONS]"
    echo ""
    echo "Commands:"
    echo "  start         Start the test network"
    echo "  stop          Stop the test network"
    echo "  restart       Restart the test network"
    echo "  status        Check the status of the network"
    echo "  clear         Clear all test data"
    echo "  logs          Show logs (use with -b for beacon or -m for miner)"
    echo ""
    echo "Options (for start command):"
    echo "  -n NUM        Number of miners (default: 3)"
    echo "  -p PORT       Beacon port (default: 8517)"
    echo "  -d            Enable debug mode"
    echo ""
    echo "Options (for logs command):"
    echo "  -b            Show beacon logs"
    echo "  -m NUM        Show miner logs (specify miner number)"
    echo "  -f            Follow logs (tail -f)"
    echo ""
    echo "Examples:"
    echo "  $0 start -n 5 -d    # Start 1 beacon and 5 miners with debug logging"
    echo "  $0 stop             # Stop the test network"
    echo "  $0 status           # Check network status"
    echo "  $0 logs -b -f       # Follow beacon logs"
    echo "  $0 logs -m 1        # Show miner1 logs"
    echo "  $0 clear            # Clear all test data"
}

# Clean existing test data
clean_test_data() {
    if [ -d "$TEST_DIR" ]; then
        echo -e "${YELLOW}Cleaning existing test data...${NC}"
        rm -rf "$TEST_DIR"
        echo -e "${GREEN}✓ Test data cleared${NC}"
    else
        echo -e "${BLUE}No test data to clean${NC}"
    fi
}

# Save PID to file
save_pid() {
    local name=$1
    local pid=$2
    mkdir -p "$(dirname "$PID_FILE")"
    echo "$name:$pid" >> "$PID_FILE"
}

# Read PIDs from file
read_pids() {
    if [ -f "$PID_FILE" ]; then
        cat "$PID_FILE"
    fi
}

# Stop the network
stop_network() {
    echo -e "${YELLOW}Stopping test network...${NC}"
    
    if [ ! -f "$PID_FILE" ]; then
        echo -e "${BLUE}No PID file found. Network may not be running.${NC}"
        return 0
    fi
    
    local stopped=0
    while IFS=: read -r name pid; do
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${BLUE}Stopping $name (PID: $pid)${NC}"
            kill "$pid" 2>/dev/null || true
            stopped=$((stopped + 1))
        fi
    done < "$PID_FILE"
    
    # Wait for graceful shutdown
    sleep 2
    
    # Force kill if still running
    while IFS=: read -r name pid; do
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${RED}Force killing $name (PID: $pid)${NC}"
            kill -9 "$pid" 2>/dev/null || true
        fi
    done < "$PID_FILE"
    
    rm -f "$PID_FILE"
    
    if [ $stopped -gt 0 ]; then
        echo -e "${GREEN}✓ Test network stopped ($stopped processes)${NC}"
    else
        echo -e "${BLUE}No running processes found${NC}"
    fi
}

# Check network status
check_status() {
    echo -e "${BLUE}╔═══════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║   Test Network Status                     ║${NC}"
    echo -e "${BLUE}╚═══════════════════════════════════════════╝${NC}"
    echo ""
    
    if [ ! -f "$PID_FILE" ]; then
        echo -e "${YELLOW}Network is not running (no PID file found)${NC}"
        return 0
    fi
    
    local running=0
    local stopped=0
    
    while IFS=: read -r name pid; do
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${GREEN}✓ $name (PID: $pid) - RUNNING${NC}"
            running=$((running + 1))
        else
            echo -e "${RED}✗ $name (PID: $pid) - STOPPED${NC}"
            stopped=$((stopped + 1))
        fi
    done < "$PID_FILE"
    
    echo ""
    echo -e "${BLUE}Summary: $running running, $stopped stopped${NC}"
    
    if [ $stopped -gt 0 ]; then
        echo -e "${YELLOW}Warning: Some processes are not running. Consider restarting.${NC}"
    fi
}

# Show logs
show_logs() {
    local show_beacon=false
    local miner_num=""
    local follow=false
    
    shift # Remove 'logs' command
    
    while getopts "bm:f" opt; do
        case $opt in
            b)
                show_beacon=true
                ;;
            m)
                miner_num=$OPTARG
                ;;
            f)
                follow=true
                ;;
            *)
                usage
                exit 1
                ;;
        esac
    done
    
    if [ "$show_beacon" = true ]; then
        local beacon_log="${TEST_DIR}/beacon/beacon.log"
        if [ -f "$beacon_log" ]; then
            if [ "$follow" = true ]; then
                echo -e "${BLUE}Following beacon logs (Ctrl+C to exit)...${NC}"
                tail -f "$beacon_log"
            else
                echo -e "${BLUE}Beacon logs:${NC}"
                cat "$beacon_log"
            fi
        else
            echo -e "${RED}Beacon log file not found: $beacon_log${NC}"
        fi
    elif [ -n "$miner_num" ]; then
        local miner_log="${TEST_DIR}/miner${miner_num}/miner.log"
        if [ -f "$miner_log" ]; then
            if [ "$follow" = true ]; then
                echo -e "${BLUE}Following miner${miner_num} logs (Ctrl+C to exit)...${NC}"
                tail -f "$miner_log"
            else
                echo -e "${BLUE}Miner${miner_num} logs:${NC}"
                cat "$miner_log"
            fi
        else
            echo -e "${RED}Miner log file not found: $miner_log${NC}"
        fi
    else
        echo -e "${YELLOW}Please specify -b for beacon or -m NUM for miner${NC}"
        usage
        exit 1
    fi
}

# Parse command line arguments for start command
parse_start_arguments() {
    shift # Remove 'start' command
    
    while getopts "n:p:dh" opt; do
        case $opt in
            n)
                NUM_MINERS=$OPTARG
                ;;
            p)
                BEACON_PORT=$OPTARG
                ;;
            d)
                DEBUG_MODE=true
                ;;
            h)
                usage
                exit 0
                ;;
            *)
                usage
                exit 1
                ;;
        esac
    done
}

# Verify build environment
verify_build_environment() {
    # Check if build directory exists
    if [ ! -d "$BUILD_DIR" ]; then
        echo -e "${RED}Error: Build directory not found: $BUILD_DIR${NC}"
        echo -e "${YELLOW}Please run: mkdir build && cd build && cmake .. && make${NC}"
        exit 1
    fi

    # Check if executables exist
    if [ ! -f "$BUILD_DIR/app/pp-beacon" ]; then
        echo -e "${RED}Error: pp-beacon executable not found${NC}"
        echo -e "${YELLOW}Please build the project first: cd build && make${NC}"
        exit 1
    fi

    if [ ! -f "$BUILD_DIR/app/pp-miner" ]; then
        echo -e "${RED}Error: pp-miner executable not found${NC}"
        echo -e "${YELLOW}Please build the project first: cd build && make${NC}"
        exit 1
    fi
}

# Print welcome banner
print_banner() {
    echo -e "${GREEN}╔═══════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║   pp-ledger Test Network Startup         ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${BLUE}Configuration:${NC}"
    echo -e "  Build directory: ${BUILD_DIR}"
    echo -e "  Test directory:  ${TEST_DIR}"
    echo -e "  Beacon port:     ${BEACON_PORT}"
    echo -e "  Number of miners: ${NUM_MINERS}"
    echo -e "  Miner ports:     ${MINER_BASE_PORT}-$((MINER_BASE_PORT + NUM_MINERS - 1))"
    echo -e "  Debug mode:      ${DEBUG_MODE}"
    echo ""
}

# Get debug flag for executables
get_debug_flag() {
    if [ "$DEBUG_MODE" = true ]; then
        echo "--debug"
    else
        echo ""
    fi
}

# Initialize beacon server
initialize_beacon() {
    local beacon_dir="${TEST_DIR}/beacon"
    
    if [ ! -f "$beacon_dir/.signature" ]; then
        echo -e "${YELLOW}Initializing beacon server...${NC}"
        
        # Remove existing incomplete beacon directory
        if [ -d "$beacon_dir" ]; then
            rm -rf "$beacon_dir"
        fi
        
        # Initialize beacon (it will create the directory)
        run_cmd "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" --init $(get_debug_flag)
        
        if [ $? -ne 0 ]; then
            echo -e "${RED}Failed to initialize beacon${NC}"
            exit 1
        fi
        echo -e "${GREEN}✓ Beacon initialized${NC}"
    else
        echo -e "${BLUE}Using existing beacon data${NC}"
    fi
}

# Create beacon configuration file
create_beacon_config() {
    local beacon_dir="${TEST_DIR}/beacon"
    
    cat > "$beacon_dir/config.json" << EOF
{
  "host": "localhost",
  "port": $BEACON_PORT
}
EOF
}

# Start beacon server
start_beacon() {
    local beacon_dir="${TEST_DIR}/beacon"
    
    echo -e "${YELLOW}Starting beacon server on port ${BEACON_PORT}...${NC}"
    run_bg_cmd "$beacon_dir/console.log" "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" $(get_debug_flag)
    BEACON_PID=$!
    save_pid "beacon" $BEACON_PID
    
    # Wait for beacon to start
    sleep 2
    
    if ! kill -0 $BEACON_PID 2>/dev/null; then
        echo -e "${RED}Failed to start beacon server${NC}"
        cat "$beacon_dir/console.log"
        exit 1
    fi
    
    echo -e "${GREEN}✓ Beacon server running (PID: $BEACON_PID)${NC}"
}

# Create miner key file and configuration file
create_miner_config() {
    local miner_id=$1
    local miner_dir=$2
    local miner_port=$3
    
    local private_key=$(printf 'test_private_key_miner%d_DO_NOT_USE_IN_PRODUCTION' $miner_id)
    local key_file="$miner_dir/key.txt"
    
    # Miner server requires a key file; write the key into key.txt
    printf '%s' "$private_key" > "$key_file"
    
    # Use absolute path for key so it works regardless of process cwd
    local key_path="$key_file"
    
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

# Initialize and start a single miner
start_miner() {
    local miner_id=$1
    local miner_dir="${TEST_DIR}/miner${miner_id}"
    local miner_port=$((MINER_BASE_PORT + miner_id - 1))
    
    echo -e "${YELLOW}Starting miner${miner_id} on port ${miner_port}...${NC}"
    
    # Clean up existing incomplete miner directory (without signature file)
    if [ -d "$miner_dir" ] && [ ! -f "$miner_dir/.signature" ]; then
        rm -rf "$miner_dir"
    fi
    
    # If miner directory doesn't exist, let the miner create it first (creates .signature and default config)
    if [ ! -d "$miner_dir" ]; then
        echo -e "${BLUE}Running: $BUILD_DIR/app/pp-miner -d $miner_dir (init, output suppressed)${NC}"
        "$BUILD_DIR/app/pp-miner" -d "$miner_dir" >&/dev/null || true
    fi
    
    # Always write key file and config (miner requires "key" pointing to a key file)
    create_miner_config $miner_id "$miner_dir" $miner_port
    
    # Start miner with the updated config
    run_bg_cmd "${miner_dir}/console.log" "$BUILD_DIR/app/pp-miner" -d "$miner_dir" $(get_debug_flag)
    local miner_pid=$!
    save_pid "miner${miner_id}" $miner_pid
    
    # Brief wait before next miner
    sleep 2
    
    if ! kill -0 $miner_pid 2>/dev/null; then
        echo -e "${RED}Failed to start miner${miner_id}${NC}"
        cat "${miner_dir}/console.log"
        stop_network
        exit 1
    fi
    
    echo -e "${GREEN}✓ Miner${miner_id} running (PID: $miner_pid)${NC}"
}

# Start all miners
start_miners() {
    for i in $(seq 1 $NUM_MINERS); do
        start_miner $i
    done
}

# Print success message and usage instructions
print_success_message() {
    echo ""
    echo -e "${GREEN}╔═══════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║   Test Network Started Successfully!      ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${BLUE}Network is running in the background.${NC}"
    echo ""
    echo -e "${BLUE}Control Commands:${NC}"
    echo -e "  ${YELLOW}$0 status${NC}    # Check network status"
    echo -e "  ${YELLOW}$0 stop${NC}      # Stop the network"
    echo -e "  ${YELLOW}$0 logs -b -f${NC} # Follow beacon logs"
    echo -e "  ${YELLOW}$0 logs -m 1${NC}  # Show miner1 logs"
    echo ""
    echo -e "${BLUE}Quick Client Commands:${NC}"
    echo -e "  ${YELLOW}# Check beacon status${NC}"
    echo -e "  $BUILD_DIR/app/pp-client -b status"
    echo ""
    echo -e "  ${YELLOW}# Check miner1 status${NC}"
    echo -e "  $BUILD_DIR/app/pp-client -m status"
    echo ""
    echo -e "  ${YELLOW}# Add a transaction${NC}"
    echo -e "  $BUILD_DIR/app/pp-client -m add-tx alice bob 100"
    echo ""
    echo -e "  ${YELLOW}# Check other miners (use different ports)${NC}"
    echo -e "  $BUILD_DIR/app/pp-client -m -p 8519 status  # miner2"
    echo -e "  $BUILD_DIR/app/pp-client -m -p 8520 status  # miner3"
    echo ""
    echo -e "${BLUE}Log Files:${NC}"
    echo -e "  Beacon:  ${TEST_DIR}/beacon/beacon.log"
    for i in $(seq 1 $NUM_MINERS); do
        echo -e "  Miner${i}:  ${TEST_DIR}/miner${i}/miner.log"
    done
    echo ""
}

# Start the network
start_network() {
    # Check if network is already running
    if [ -f "$PID_FILE" ]; then
        echo -e "${YELLOW}Network appears to be running. Checking status...${NC}"
        check_status
        echo ""
        echo -e "${YELLOW}Stop the network first with: $0 stop${NC}"
        exit 1
    fi
    
    verify_build_environment
    
    # Create test directory
    mkdir -p "$TEST_DIR"
    
    print_banner
    
    # Set up beacon
    initialize_beacon
    create_beacon_config
    start_beacon
    
    # Set up miners
    start_miners
    
    # Show success message
    print_success_message
}

# Restart the network
restart_network() {
    echo -e "${BLUE}Restarting test network...${NC}"
    stop_network
    sleep 1
    start_network
}

# Main execution
main() {
    if [ $# -eq 0 ]; then
        usage
        exit 1
    fi
    
    COMMAND=$1
    
    case "$COMMAND" in
        start)
            parse_start_arguments "$@"
            start_network
            ;;
        stop)
            stop_network
            ;;
        restart)
            shift
            parse_start_arguments "start" "$@"
            restart_network
            ;;
        status)
            check_status
            ;;
        clear)
            clean_test_data
            ;;
        logs)
            show_logs "$@"
            ;;
        -h|--help|help)
            usage
            ;;
        *)
            echo -e "${RED}Unknown command: $COMMAND${NC}"
            echo ""
            usage
            exit 1
            ;;
    esac
}

# Run main function
main "$@"
