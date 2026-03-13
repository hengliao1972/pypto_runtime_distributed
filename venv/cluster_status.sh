#!/bin/bash
# =============================================================================
# Linqu Virtual Cluster Status Report
#
# Displays the current state of the 1024-node virtual cluster.
# =============================================================================
set -euo pipefail

SYSTEM_NAME="${LINQU_SYSTEM:-linqu_vcluster_1024}"
BASE_DIR="${LINQU_BASE:-/tmp/linqu/${SYSTEM_NAME}}"

while [[ $# -gt 0 ]]; do
    case $1 in
        --base) BASE_DIR="$2"; shift 2;;
        -h|--help) echo "Usage: $0 [--base DIR]"; exit 0;;
        *)      echo "Unknown option: $1"; exit 1;;
    esac
done

if [[ ! -d "$BASE_DIR" ]]; then
    echo "Cluster not found at ${BASE_DIR}"
    echo "Run launch_cluster.sh first."
    exit 1
fi

echo "============================================================"
echo "  Linqu Virtual Cluster Status"
echo "  Base: ${BASE_DIR}"
echo "============================================================"

# Count by level
TOTAL_DIRS=$(find "$BASE_DIR" -maxdepth 4 -mindepth 4 -type d 2>/dev/null | wc -l)
TOTAL_SOCKETS=$(find "$BASE_DIR" -name "daemon.sock" 2>/dev/null | wc -l)
TOTAL_PIDS=$(find "$BASE_DIR" -name "daemon.pid" 2>/dev/null | wc -l)
ALIVE=0
DEAD=0

while IFS= read -r pid_file; do
    pid=$(cat "$pid_file" 2>/dev/null)
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        ALIVE=$((ALIVE + 1))
    else
        DEAD=$((DEAD + 1))
    fi
done < <(find "$BASE_DIR" -name "daemon.pid" 2>/dev/null)

echo ""
echo "  Node directories:    ${TOTAL_DIRS}"
echo "  Socket files:        ${TOTAL_SOCKETS}"
echo "  PID files:           ${TOTAL_PIDS}"
echo "  Processes alive:     ${ALIVE}"
echo "  Processes dead:      ${DEAD}"
echo ""

# Per-L5 breakdown
echo "  Per-Supernode (L5) breakdown:"
echo "  ┌────────┬────────┬─────────┬───────┐"
echo "  │   L5   │  Hosts │ Sockets │ Alive │"
echo "  ├────────┼────────┼─────────┼───────┤"

for l5 in $(seq 0 15); do
    L5_DIR="${BASE_DIR}/L6_0/L5_${l5}"
    if [[ -d "$L5_DIR" ]]; then
        L5_HOSTS=$(find "$L5_DIR" -maxdepth 2 -mindepth 2 -type d 2>/dev/null | wc -l)
        L5_SOCKS=$(find "$L5_DIR" -name "daemon.sock" 2>/dev/null | wc -l)
        L5_ALIVE=0
        while IFS= read -r pf; do
            p=$(cat "$pf" 2>/dev/null)
            if [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null; then
                L5_ALIVE=$((L5_ALIVE + 1))
            fi
        done < <(find "$L5_DIR" -name "daemon.pid" 2>/dev/null)
        printf "  │ L5_%-3s │  %4d  │  %5d  │ %5d │\n" "$l5" "$L5_HOSTS" "$L5_SOCKS" "$L5_ALIVE"
    fi
done

echo "  └────────┴────────┴─────────┴───────┘"
echo ""

# Disk usage
DISK_USAGE=$(du -sh "$BASE_DIR" 2>/dev/null | cut -f1)
echo "  Total disk usage:    ${DISK_USAGE}"

# Socket file sample
echo ""
echo "  Sample socket paths (first 5):"
find "$BASE_DIR" -name "daemon.sock" 2>/dev/null | head -5 | sed 's/^/    /'
echo ""
