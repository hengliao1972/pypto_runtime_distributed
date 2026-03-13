#!/bin/bash
# test_single_pod.sh — Milestone 14.2
# Launch 16 daemon processes in one pod, verify discovery.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON="${PROJECT_DIR}/build/linqu_daemon"
ORCH="${PROJECT_DIR}/build/linqu_orchestrator"
BASE="/tmp/linqu_test_pod_$$"
PASS=0
FAIL=0
NUM_HOSTS=16

cleanup() {
    kill $(jobs -p) 2>/dev/null || true
    sleep 1
    rm -rf "$BASE"
}
trap cleanup EXIT

echo "=== test_single_pod: 16 hosts in one pod ==="

rm -rf "$BASE"

# Launch 16 L3 daemons in pod (l5=0, l4=0, l3=0..15)
for l3 in $(seq 0 $((NUM_HOSTS - 1))); do
    LINQU_LEVEL=3 LINQU_L6=0 LINQU_L5=0 LINQU_L4=0 LINQU_L3=$l3 LINQU_BASE="$BASE" \
        "$DAEMON" > /dev/null 2>&1 &
done
sleep 2

# Check 1: all 16 socket files exist
SOCKETS=$(find "$BASE" -name "daemon_L3.sock" 2>/dev/null | wc -l)
if [[ "$SOCKETS" -eq "$NUM_HOSTS" ]]; then
    echo "  [PASS] Check 1: $SOCKETS/$NUM_HOSTS socket files exist"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 1: only $SOCKETS/$NUM_HOSTS sockets"
    FAIL=$((FAIL + 1))
fi

# Check 2: orchestrator discovers all 16
OUTPUT=$(LINQU_BASE="$BASE" "$ORCH" --command discover 2>/dev/null)
L3_COUNT=$(echo "$OUTPUT" | python3 -c "import sys,json; print(json.load(sys.stdin)['l3_hosts'])" 2>/dev/null || echo 0)
if [[ "$L3_COUNT" -eq "$NUM_HOSTS" ]]; then
    echo "  [PASS] Check 2: orchestrator discovered $L3_COUNT hosts"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 2: discovered $L3_COUNT/$NUM_HOSTS hosts"
    FAIL=$((FAIL + 1))
fi

# Check 3: all daemons are unique processes
PIDS=$(jobs -p | sort -u | wc -l)
if [[ "$PIDS" -eq "$NUM_HOSTS" ]]; then
    echo "  [PASS] Check 3: $PIDS unique daemon PIDs"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 3: $PIDS PIDs (expected $NUM_HOSTS)"
    FAIL=$((FAIL + 1))
fi

# Check 4: shutdown all
LINQU_BASE="$BASE" "$ORCH" --command shutdown 2>/dev/null
sleep 3
ALIVE=0
for pid in $(jobs -p 2>/dev/null); do
    kill -0 "$pid" 2>/dev/null && ALIVE=$((ALIVE + 1))
done
if [[ $ALIVE -eq 0 ]]; then
    echo "  [PASS] Check 4: all daemons shut down cleanly"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 4: $ALIVE daemons still running"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: ${PASS} PASS, ${FAIL} FAIL ==="
[[ $FAIL -eq 0 ]] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL
