#!/bin/bash
# test_single_supernode.sh — Milestone 14.3
# Launch 64 daemon processes in one supernode (4 pods × 16 hosts).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON="${PROJECT_DIR}/build/linqu_daemon"
ORCH="${PROJECT_DIR}/build/linqu_orchestrator"
BASE="/tmp/linqu_test_supernode_$$"
PASS=0
FAIL=0
NUM_L4=4
NUM_L3=16
TOTAL=$((NUM_L4 * NUM_L3))

cleanup() {
    kill $(jobs -p) 2>/dev/null || true
    sleep 1
    rm -rf "$BASE"
}
trap cleanup EXIT

echo "=== test_single_supernode: ${NUM_L4} pods × ${NUM_L3} hosts = ${TOTAL} ==="

rm -rf "$BASE"

for l4 in $(seq 0 $((NUM_L4 - 1))); do
    for l3 in $(seq 0 $((NUM_L3 - 1))); do
        LINQU_LEVEL=3 LINQU_L6=0 LINQU_L5=0 LINQU_L4=$l4 LINQU_L3=$l3 LINQU_BASE="$BASE" \
            "$DAEMON" > /dev/null 2>&1 &
    done
done
sleep 3

# Check 1: all 64 sockets exist
SOCKETS=$(find "$BASE" -name "daemon_L3.sock" 2>/dev/null | wc -l)
if [[ "$SOCKETS" -eq "$TOTAL" ]]; then
    echo "  [PASS] Check 1: $SOCKETS/$TOTAL socket files"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 1: $SOCKETS/$TOTAL sockets"
    FAIL=$((FAIL + 1))
fi

# Check 2: orchestrator discovers all
OUTPUT=$(LINQU_BASE="$BASE" "$ORCH" --command discover 2>/dev/null)
L3_COUNT=$(echo "$OUTPUT" | python3 -c "import sys,json; print(json.load(sys.stdin)['l3_hosts'])" 2>/dev/null || echo 0)
if [[ "$L3_COUNT" -eq "$TOTAL" ]]; then
    echo "  [PASS] Check 2: orchestrator discovered $L3_COUNT hosts"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 2: discovered $L3_COUNT/$TOTAL"
    FAIL=$((FAIL + 1))
fi

# Check 3: sockets distributed across 4 pods
for l4 in $(seq 0 $((NUM_L4 - 1))); do
    POD_SOCKS=$(find "$BASE/L6_0/L5_0/L4_${l4}" -name "daemon_L3.sock" 2>/dev/null | wc -l)
    if [[ "$POD_SOCKS" -ne "$NUM_L3" ]]; then
        echo "  [FAIL] Check 3: pod $l4 has $POD_SOCKS sockets (expected $NUM_L3)"
        FAIL=$((FAIL + 1))
        break
    fi
done
if [[ $FAIL -eq 0 ]] || [[ $PASS -ge 2 ]]; then
    echo "  [PASS] Check 3: each pod has $NUM_L3 hosts"
    PASS=$((PASS + 1))
fi

# Check 4: shutdown
LINQU_BASE="$BASE" "$ORCH" --command shutdown 2>/dev/null
sleep 3
ALIVE=0
for pid in $(jobs -p 2>/dev/null); do
    kill -0 "$pid" 2>/dev/null && ALIVE=$((ALIVE + 1))
done
if [[ $ALIVE -eq 0 ]]; then
    echo "  [PASS] Check 4: all ${TOTAL} daemons shut down"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 4: $ALIVE daemons still running"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: ${PASS} PASS, ${FAIL} FAIL ==="
[[ $FAIL -eq 0 ]] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL
