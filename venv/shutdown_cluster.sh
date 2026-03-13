#!/bin/bash
# =============================================================================
# Linqu Virtual Cluster Shutdown
#
# Gracefully shuts down all daemon processes launched by launch_cluster.sh.
# =============================================================================
set -euo pipefail

SYSTEM_NAME="${LINQU_SYSTEM:-linqu_vcluster_1024}"
BASE_DIR="${LINQU_BASE:-/tmp/linqu/${SYSTEM_NAME}}"
FORCE=0

while [[ $# -gt 0 ]]; do
    case $1 in
        --base)  BASE_DIR="$2"; shift 2;;
        --force) FORCE=1; shift;;
        -h|--help) echo "Usage: $0 [--base DIR] [--force]"; exit 0;;
        *)       echo "Unknown option: $1"; exit 1;;
    esac
done

echo "Shutting down Linqu cluster at ${BASE_DIR}..."

# Method 1: Use saved PID file
PID_FILE="${BASE_DIR}/cluster.pids"
if [[ -f "$PID_FILE" ]]; then
    PIDS=$(cat "$PID_FILE")
    COUNT=$(echo "$PIDS" | wc -l)
    echo "  Found ${COUNT} PIDs in ${PID_FILE}"

    if [[ $FORCE -eq 1 ]]; then
        echo "$PIDS" | xargs -r kill -9 2>/dev/null || true
    else
        echo "$PIDS" | xargs -r kill -TERM 2>/dev/null || true
        sleep 2
        # Kill survivors
        ALIVE=$(echo "$PIDS" | xargs -I{} sh -c 'kill -0 {} 2>/dev/null && echo {}' 2>/dev/null || true)
        if [[ -n "$ALIVE" ]]; then
            echo "  Force-killing $(echo "$ALIVE" | wc -l) remaining processes..."
            echo "$ALIVE" | xargs -r kill -9 2>/dev/null || true
        fi
    fi
fi

# Method 2: Kill by PID files in node directories
find "$BASE_DIR" -name "daemon.pid" -exec cat {} \; 2>/dev/null | \
    xargs -r kill -9 2>/dev/null || true

# Verify
sleep 1
REMAINING=$(find "$BASE_DIR" -name "daemon.pid" -exec cat {} \; 2>/dev/null | \
    xargs -I{} sh -c 'kill -0 {} 2>/dev/null && echo 1' 2>/dev/null | wc -l)

echo "  Remaining processes: ${REMAINING}"

# Clean socket files
SOCKETS=$(find "$BASE_DIR" -name "daemon.sock" 2>/dev/null | wc -l)
if [[ $SOCKETS -gt 0 ]]; then
    echo "  Cleaning ${SOCKETS} socket files..."
    find "$BASE_DIR" -name "daemon.sock" -delete
fi

echo "Cluster shutdown complete."
echo ""
echo "To clean all state: rm -rf ${BASE_DIR}"
