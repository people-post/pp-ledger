#!/bin/bash

# pp-ledger Test Network Startup Script
# This script starts a test network with 1 beacon and multiple miners

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TEST_DIR="${BUILD_DIR}/test-network"
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

# PID tracking
declare -a PIDS=()

# Cleanup function
cleanup() {
    echo -e "\n${YELLOW}Shutting down test network...${NC}"
    
    # Kill all processes
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${BLUE}Stopping process $pid${NC}"
            kill "$pid" 2>/dev/null || true
        fi
    done
    
    # Wait a bit for graceful shutdown
    sleep 2
    
    # Force kill if still running
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${RED}Force killing process $pid${NC}"
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    
    echo -e "${GREEN}Test network stopped${NC}"
    exit 0
}

# Set up signal handlers
trap cleanup SIGINT SIGTERM

# Print usage
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -n NUM        Number of miners (default: 3)"
    echo "  -p PORT       Beacon port (default: 8517)"
    echo "  -d            Enable debug mode"
    echo "  -c            Clean existing test data before starting"
    echo "  -h            Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 -n 5 -d    # Start 1 beacon and 5 miners with debug logging"
}

# Clean existing test data
clean_test_data() {
    if [ -d "$TEST_DIR" ]; then
        echo -e "${YELLOW}Cleaning existing test data...${NC}"
        rm -rf "$TEST_DIR"
    fi
}

# Parse command line arguments
parse_arguments() {
    while getopts "n:p:dch" opt; do
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
            c)
                clean_test_data
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
        "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" --init $(get_debug_flag)
        
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
  "port": $BEACON_PORT,
  "beacons": []
}
EOF
}

# Start beacon server
start_beacon() {
    local beacon_dir="${TEST_DIR}/beacon"
    
    echo -e "${YELLOW}Starting beacon server on port ${BEACON_PORT}...${NC}"
    "$BUILD_DIR/app/pp-beacon" -d "$beacon_dir" $(get_debug_flag) > "$beacon_dir/console.log" 2>&1 &
    BEACON_PID=$!
    PIDS+=($BEACON_PID)
    
    # Wait for beacon to start
    sleep 2
    
    if ! kill -0 $BEACON_PID 2>/dev/null; then
        echo -e "${RED}Failed to start beacon server${NC}"
        cat "$beacon_dir/console.log"
        exit 1
    fi
    
    echo -e "${GREEN}✓ Beacon server running (PID: $BEACON_PID)${NC}"
}

# Create miner configuration file
create_miner_config() {
    local miner_id=$1
    local miner_dir=$2
    local miner_port=$3
    
    local private_key=$(printf 'test_private_key_miner%d_DO_NOT_USE_IN_PRODUCTION' $miner_id)
    
    cat > "$miner_dir/config.json" << EOF
{
  "minerId": $miner_id,
  "privateKey": "$private_key",
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
    
    # If miner directory doesn't exist, let the miner create it first with default config
    # Then update the config with proper values
    if [ ! -d "$miner_dir" ]; then
        # Let miner create default directory structure (will fail but create files)
        "$BUILD_DIR/app/pp-miner" -d "$miner_dir" >&/dev/null || true
        
        # Now update the config with proper values
        create_miner_config $miner_id "$miner_dir" $miner_port
    fi
    
    # Start miner with the updated config
    "$BUILD_DIR/app/pp-miner" -d "$miner_dir" $(get_debug_flag) > "${miner_dir}/console.log" 2>&1 &
    local miner_pid=$!
    PIDS+=($miner_pid)
    
    # Brief wait before next miner
    sleep 2
    
    if ! kill -0 $miner_pid 2>/dev/null; then
        echo -e "${RED}Failed to start miner${miner_id}${NC}"
        cat "${miner_dir}/console.log"
        cleanup
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
    echo -e "${GREEN}║   Test Network Running Successfully!      ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${BLUE}Quick Commands:${NC}"
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
    echo -e "${BLUE}Logs:${NC}"
    echo -e "  Beacon:  ${TEST_DIR}/beacon/beacon.log"
    for i in $(seq 1 $NUM_MINERS); do
        echo -e "  Miner${i}:  ${TEST_DIR}/miner${i}/miner.log"
    done
    echo ""
    echo -e "${BLUE}Console Output:${NC}"
    echo -e "  Beacon:  ${TEST_DIR}/beacon/console.log"
    for i in $(seq 1 $NUM_MINERS); do
        echo -e "  Miner${i}:  ${TEST_DIR}/miner${i}/console.log"
    done
    echo ""
    echo -e "${RED}Press Ctrl+C to stop all nodes${NC}"
    echo ""
}

# Main execution
main() {
    parse_arguments "$@"
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
    
    # Wait for all processes
    wait
}

# Run main function
main "$@"
