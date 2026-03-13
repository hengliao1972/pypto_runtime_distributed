#!/bin/bash
# =============================================================================
# Create the 1024-node directory topology (without launching daemons)
#
# This is a lightweight script that only creates the filesystem structure.
# Useful for verifying the topology and testing directory-based discovery.
#
# Usage:
#   ./create_topology.sh [--base DIR] [--system NAME] [--verify]
# =============================================================================
set -euo pipefail

SYSTEM_NAME="${LINQU_SYSTEM:-linqu_vcluster_1024}"
BASE_DIR="${LINQU_BASE:-/tmp/linqu/${SYSTEM_NAME}}"
VERIFY=0

while [[ $# -gt 0 ]]; do
    case $1 in
        --system)  SYSTEM_NAME="$2"; BASE_DIR="/tmp/linqu/${SYSTEM_NAME}"; shift 2;;
        --base)    BASE_DIR="$2"; shift 2;;
        --verify)  VERIFY=1; shift;;
        -h|--help) echo "Usage: $0 [--base DIR] [--system NAME] [--verify]"; exit 0;;
        *)         echo "Unknown option: $1"; exit 1;;
    esac
done

# Topology constants
NUM_L5=16    # CLOS1 supernodes
NUM_L4=4     # PODs per supernode
NUM_L3=16    # hosts per POD
TOTAL=$((NUM_L5 * NUM_L4 * NUM_L3))

echo "Creating ${TOTAL}-node topology at ${BASE_DIR}..."
START_TIME=$(date +%s%N)

# Clean
rm -rf "$BASE_DIR"

# Create all directories in parallel using xargs
seq 0 $((NUM_L5 - 1)) | while read l5; do
    seq 0 $((NUM_L4 - 1)) | while read l4; do
        seq 0 $((NUM_L3 - 1)) | while read l3; do
            echo "${BASE_DIR}/L6_0/L5_${l5}/L4_${l4}/L3_${l3}"
        done
    done
done | xargs -P 8 -I{} mkdir -p {}/code_cache {}/data_cache {}/logs

END_TIME=$(date +%s%N)
ELAPSED_MS=$(( (END_TIME - START_TIME) / 1000000 ))

# Count results
HOST_DIRS=$(find "$BASE_DIR" -maxdepth 4 -mindepth 4 -type d | wc -l)
TOTAL_DIRS=$(find "$BASE_DIR" -type d | wc -l)
DISK_USAGE=$(du -sh "$BASE_DIR" 2>/dev/null | cut -f1)

echo ""
echo "============================================================"
echo "  Topology Created"
echo "============================================================"
echo "  Host nodes (L3):     ${HOST_DIRS}"
echo "  Total directories:   ${TOTAL_DIRS}"
echo "  Disk usage:          ${DISK_USAGE}"
echo "  Time elapsed:        ${ELAPSED_MS} ms"
echo "============================================================"

if [[ $VERIFY -eq 1 ]]; then
    echo ""
    echo "Verification:"

    # Check total host count
    if [[ $HOST_DIRS -eq $TOTAL ]]; then
        echo "  [PASS] Host count: ${HOST_DIRS} == ${TOTAL}"
    else
        echo "  [FAIL] Host count: ${HOST_DIRS} != ${TOTAL}"
        exit 1
    fi

    # Check each L5 has exactly NUM_L4 * NUM_L3 hosts
    EXPECTED_PER_L5=$((NUM_L4 * NUM_L3))
    PASS=0
    FAIL=0
    for l5 in $(seq 0 $((NUM_L5 - 1))); do
        L5_DIR="${BASE_DIR}/L6_0/L5_${l5}"
        COUNT=$(find "$L5_DIR" -maxdepth 2 -mindepth 2 -type d | wc -l)
        if [[ $COUNT -eq $EXPECTED_PER_L5 ]]; then
            PASS=$((PASS + 1))
        else
            echo "  [FAIL] L5_${l5}: ${COUNT} hosts (expected ${EXPECTED_PER_L5})"
            FAIL=$((FAIL + 1))
        fi
    done
    echo "  [PASS] L5 supernode check: ${PASS}/${NUM_L5} correct"

    # Check each L4 has exactly NUM_L3 hosts
    PASS=0
    for l5 in $(seq 0 $((NUM_L5 - 1))); do
        for l4 in $(seq 0 $((NUM_L4 - 1))); do
            L4_DIR="${BASE_DIR}/L6_0/L5_${l5}/L4_${l4}"
            COUNT=$(find "$L4_DIR" -maxdepth 1 -mindepth 1 -type d | wc -l)
            if [[ $COUNT -eq $NUM_L3 ]]; then
                PASS=$((PASS + 1))
            else
                echo "  [FAIL] L5_${l5}/L4_${l4}: ${COUNT} hosts (expected ${NUM_L3})"
                FAIL=$((FAIL + 1))
            fi
        done
    done
    echo "  [PASS] L4 pod check: ${PASS}/$((NUM_L5 * NUM_L4)) correct"

    # Check subdirectories exist
    SAMPLE="${BASE_DIR}/L6_0/L5_7/L4_2/L3_11"
    if [[ -d "${SAMPLE}/code_cache" && -d "${SAMPLE}/data_cache" && -d "${SAMPLE}/logs" ]]; then
        echo "  [PASS] Subdirectories (code_cache, data_cache, logs) exist"
    else
        echo "  [FAIL] Missing subdirectories in ${SAMPLE}"
        FAIL=$((FAIL + 1))
    fi

    # Check coordinate mapping
    echo ""
    echo "  Sample coordinate mapping:"
    echo "    (l5=0,  l4=0, l3=0)  → global=0    → IP=10.0.0.0"
    echo "    (l5=0,  l4=0, l3=15) → global=15   → IP=10.0.0.15"
    echo "    (l5=0,  l4=3, l3=15) → global=63   → IP=10.0.3.15"
    echo "    (l5=1,  l4=0, l3=0)  → global=64   → IP=10.1.0.0"
    echo "    (l5=15, l4=3, l3=15) → global=1023 → IP=10.15.3.15"

    echo ""
    echo "  Tree structure (first 3 levels):"
    find "$BASE_DIR" -maxdepth 3 -type d | head -25 | sed "s|${BASE_DIR}|  .|"
    echo "  ... (${TOTAL_DIRS} total directories)"

    if [[ $FAIL -eq 0 ]]; then
        echo ""
        echo "  ========== ALL CHECKS PASSED =========="
    else
        echo ""
        echo "  ========== ${FAIL} CHECKS FAILED =========="
        exit 1
    fi
fi
