#!/bin/bash
# =============================================================================
# Linqu Virtual Cluster Launcher — 1024-Node Configuration
#
# Topology:
#   L6: 1 cluster
#   L5: 16 CLOS1 supernodes (64 hosts each)
#   L4: 4 PODs per supernode (16 hosts each)
#   L3: 16 hosts per POD
#   Total: 16 × 4 × 16 = 1024 host processes
#
# Each host runs as a separate process on the same arm64 machine.
# Processes communicate via Unix Domain Sockets.
# Each process has its own disk storage directory.
#
# Usage:
#   ./launch_cluster.sh [--system NAME] [--base DIR] [--daemon PATH] [--dry-run]
#
# =============================================================================
set -euo pipefail

# --- Configuration -----------------------------------------------------------
SYSTEM_NAME="${LINQU_SYSTEM:-linqu_vcluster_1024}"
BASE_DIR="${LINQU_BASE:-/tmp/linqu/${SYSTEM_NAME}}"
DAEMON="${LINQU_DAEMON:-./build/linqu_daemon}"
DRY_RUN=0

# Topology constants
NUM_L5=16    # supernodes
NUM_L4=4     # pods per supernode
NUM_L3=16    # hosts per pod
TOTAL_HOSTS=$((NUM_L5 * NUM_L4 * NUM_L3))  # 1024

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --system)  SYSTEM_NAME="$2"; BASE_DIR="/tmp/linqu/${SYSTEM_NAME}"; shift 2;;
        --base)    BASE_DIR="$2"; shift 2;;
        --daemon)  DAEMON="$2"; shift 2;;
        --dry-run) DRY_RUN=1; shift;;
        -h|--help) echo "Usage: $0 [--system NAME] [--base DIR] [--daemon PATH] [--dry-run]"; exit 0;;
        *)         echo "Unknown option: $1"; exit 1;;
    esac
done

echo "============================================================"
echo "  Linqu Virtual Cluster Launcher"
echo "============================================================"
echo "  System:     ${SYSTEM_NAME}"
echo "  Base dir:   ${BASE_DIR}"
echo "  Daemon:     ${DAEMON}"
echo "  Topology:   ${NUM_L5} L5 × ${NUM_L4} L4 × ${NUM_L3} L3 = ${TOTAL_HOSTS} hosts"
echo "============================================================"

# --- Pre-flight checks -------------------------------------------------------
if [[ $DRY_RUN -eq 0 ]]; then
    if [[ ! -x "$DAEMON" ]]; then
        echo "ERROR: Daemon executable not found: ${DAEMON}"
        echo "Build it first with: cd build && cmake .. && make linqu_daemon"
        exit 1
    fi

    # Check system limits
    FD_LIMIT=$(ulimit -n)
    if [[ $FD_LIMIT -lt 32768 ]]; then
        echo "WARNING: File descriptor limit is $FD_LIMIT (need ≥32768 for 1024 nodes)"
        echo "Run: ulimit -n 65536"
    fi

    PROC_LIMIT=$(ulimit -u)
    if [[ $PROC_LIMIT -lt 4096 ]]; then
        echo "WARNING: Process limit is $PROC_LIMIT (need ≥4096)"
        echo "Run: ulimit -u 8192"
    fi
fi

# --- Clean up previous run ---------------------------------------------------
echo ""
echo "[1/4] Cleaning previous state..."
if [[ -d "$BASE_DIR" ]]; then
    # Kill any existing daemons that might have stale sockets
    find "$BASE_DIR" -name "daemon.pid" -exec cat {} \; 2>/dev/null | \
        xargs -r kill 2>/dev/null || true
    sleep 1
    rm -rf "$BASE_DIR"
fi

# --- Create directory structure ----------------------------------------------
echo "[2/4] Creating directory structure for ${TOTAL_HOSTS} nodes..."

create_node_dir() {
    local l5=$1 l4=$2 l3=$3
    local node_dir="${BASE_DIR}/L6_0/L5_${l5}/L4_${l4}/L3_${l3}"
    mkdir -p "${node_dir}/code_cache" "${node_dir}/data_cache" "${node_dir}/logs"
}

# Parallel directory creation (much faster for 1024 dirs)
for l5 in $(seq 0 $((NUM_L5 - 1))); do
    for l4 in $(seq 0 $((NUM_L4 - 1))); do
        for l3 in $(seq 0 $((NUM_L3 - 1))); do
            create_node_dir $l5 $l4 $l3
        done
    done
done

DIR_COUNT=$(find "$BASE_DIR" -type d | wc -l)
echo "  Created ${DIR_COUNT} directories"

if [[ $DRY_RUN -eq 1 ]]; then
    echo ""
    echo "[DRY RUN] Directory structure created. Showing sample:"
    echo ""
    find "$BASE_DIR" -maxdepth 5 -type d | head -30
    echo "  ... (${DIR_COUNT} total)"
    echo ""
    echo "[DRY RUN] Would launch ${TOTAL_HOSTS} daemon processes."
    echo "  Sample command:"
    echo "    LINQU_SYSTEM=${SYSTEM_NAME} LINQU_L6=0 LINQU_L5=0 LINQU_L4=0 LINQU_L3=0 \\"
    echo "    LINQU_BASE=${BASE_DIR} LINQU_USE_MOCK_CHIP=1 ${DAEMON}"
    exit 0
fi

# --- Launch daemon processes -------------------------------------------------
echo "[3/4] Launching ${TOTAL_HOSTS} daemon processes..."

PIDS=()
LAUNCHED=0
FAILED=0

launch_daemon() {
    local l5=$1 l4=$2 l3=$3
    local global_idx=$(( l5 * NUM_L4 * NUM_L3 + l4 * NUM_L3 + l3 ))
    local node_dir="${BASE_DIR}/L6_0/L5_${l5}/L4_${l4}/L3_${l3}"
    local log_file="${node_dir}/logs/daemon.log"

    LINQU_SYSTEM="$SYSTEM_NAME" \
    LINQU_L6=0 \
    LINQU_L5=$l5 \
    LINQU_L4=$l4 \
    LINQU_L3=$l3 \
    LINQU_BASE="$BASE_DIR" \
    LINQU_USE_MOCK_CHIP=1 \
    "$DAEMON" > "$log_file" 2>&1 &

    local pid=$!
    echo "$pid" > "${node_dir}/daemon.pid"
    PIDS+=($pid)
}

# Launch in batches to avoid overwhelming the OS
BATCH_SIZE=64

for l5 in $(seq 0 $((NUM_L5 - 1))); do
    for l4 in $(seq 0 $((NUM_L4 - 1))); do
        for l3 in $(seq 0 $((NUM_L3 - 1))); do
            launch_daemon $l5 $l4 $l3
            LAUNCHED=$((LAUNCHED + 1))

            # Progress reporting
            if (( LAUNCHED % BATCH_SIZE == 0 )); then
                echo "  Launched ${LAUNCHED}/${TOTAL_HOSTS} daemons..."
                sleep 0.1  # Brief pause between batches
            fi
        done
    done
done

echo "  Launched ${LAUNCHED} daemons total"

# --- Wait for daemons to initialize -----------------------------------------
echo "[4/4] Waiting for daemons to initialize..."

MAX_WAIT=30
WAIT_INTERVAL=2
ELAPSED=0

while [[ $ELAPSED -lt $MAX_WAIT ]]; do
    SOCKETS=$(find "$BASE_DIR" -name "daemon.sock" 2>/dev/null | wc -l)
    if [[ $SOCKETS -ge $TOTAL_HOSTS ]]; then
        break
    fi
    echo "  ${SOCKETS}/${TOTAL_HOSTS} daemons ready... (${ELAPSED}s)"
    sleep $WAIT_INTERVAL
    ELAPSED=$((ELAPSED + WAIT_INTERVAL))
done

SOCKETS=$(find "$BASE_DIR" -name "daemon.sock" 2>/dev/null | wc -l)
ALIVE=$(printf '%s\n' "${PIDS[@]}" | xargs -I{} sh -c 'kill -0 {} 2>/dev/null && echo 1 || true' | wc -l)

echo ""
echo "============================================================"
echo "  Cluster Status"
echo "============================================================"
echo "  Processes launched:  ${LAUNCHED}"
echo "  Processes alive:     ${ALIVE}"
echo "  Sockets ready:       ${SOCKETS}/${TOTAL_HOSTS}"
echo "  PID file:            ${BASE_DIR}/cluster.pids"
echo "============================================================"

# Save PID list for shutdown
printf '%s\n' "${PIDS[@]}" > "${BASE_DIR}/cluster.pids"

if [[ $SOCKETS -lt $TOTAL_HOSTS ]]; then
    echo ""
    echo "WARNING: Not all daemons initialized within ${MAX_WAIT}s."
    echo "Check logs: find ${BASE_DIR} -name daemon.log | head -5 | xargs tail -20"
fi

echo ""
echo "Cluster is running. To shut down:"
echo "  ./shutdown_cluster.sh --base ${BASE_DIR}"
echo ""
echo "To run the hierarchical test:"
echo "  LINQU_SYSTEM=${SYSTEM_NAME} LINQU_BASE=${BASE_DIR} \\"
echo "  ./build/linqu_orchestrator --so examples/hierarchical_vecadd/build/L6_cluster_orch.so --size 1048576 --verify"
