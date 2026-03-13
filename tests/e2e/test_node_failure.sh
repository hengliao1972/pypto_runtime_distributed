#!/bin/bash
# test_node_failure.sh — Milestone 14.10
# Launch 16 daemons, kill one, verify others survive.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON="${PROJECT_DIR}/build/linqu_daemon"
ORCH="${PROJECT_DIR}/build/linqu_orchestrator"
BASE="/tmp/linqu_test_failure_$$"
PASS=0
FAIL=0
NUM_HOSTS=16

cleanup() {
    kill $(jobs -p) 2>/dev/null || true
    sleep 1
    rm -rf "$BASE"
}
trap cleanup EXIT

echo "=== test_node_failure: kill one of ${NUM_HOSTS} daemons ==="
rm -rf "$BASE"

DAEMON_PIDS=()
for l3 in $(seq 0 $((NUM_HOSTS - 1))); do
    LINQU_LEVEL=3 LINQU_L6=0 LINQU_L5=0 LINQU_L4=0 LINQU_L3=$l3 LINQU_BASE="$BASE" \
        "$DAEMON" > /dev/null 2>&1 &
    DAEMON_PIDS+=($!)
done
sleep 2

# Check 1: all 16 alive
ALIVE=0
for pid in "${DAEMON_PIDS[@]}"; do
    kill -0 "$pid" 2>/dev/null && ALIVE=$((ALIVE + 1))
done
if [[ $ALIVE -eq $NUM_HOSTS ]]; then
    echo "  [PASS] Check 1: all $NUM_HOSTS daemons alive"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 1: only $ALIVE/$NUM_HOSTS alive"
    FAIL=$((FAIL + 1))
fi

# Kill daemon at l3=7 with SIGKILL (simulate crash)
VICTIM_PID=${DAEMON_PIDS[7]}
echo "  Killing daemon l3=7 (pid=$VICTIM_PID) with SIGKILL..."
kill -9 "$VICTIM_PID" 2>/dev/null
sleep 2

# Check 2: victim is dead
if ! kill -0 "$VICTIM_PID" 2>/dev/null; then
    echo "  [PASS] Check 2: victim (l3=7) is dead"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 2: victim still alive"
    FAIL=$((FAIL + 1))
fi

# Check 3: other 15 are still alive
ALIVE=0
for i in $(seq 0 $((NUM_HOSTS - 1))); do
    [[ $i -eq 7 ]] && continue
    kill -0 "${DAEMON_PIDS[$i]}" 2>/dev/null && ALIVE=$((ALIVE + 1))
done
if [[ $ALIVE -eq $((NUM_HOSTS - 1)) ]]; then
    echo "  [PASS] Check 3: $ALIVE remaining daemons alive"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 3: only $ALIVE/$((NUM_HOSTS - 1)) remaining"
    FAIL=$((FAIL + 1))
fi

# Check 4: orchestrator still discovers 15 alive sockets
# The dead daemon's socket file may still exist but is unreachable
OUTPUT=$(LINQU_BASE="$BASE" "$ORCH" --command discover 2>/dev/null)
DISCOVERED=$(echo "$OUTPUT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['l3_hosts'])" 2>/dev/null || echo 0)
# Socket files may still count the dead one
echo "  Discovered $DISCOVERED hosts (includes stale socket of dead node)"
if [[ "$DISCOVERED" -ge $((NUM_HOSTS - 1)) ]]; then
    echo "  [PASS] Check 4: discovery still works ($DISCOVERED hosts found)"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 4: discovery found only $DISCOVERED"
    FAIL=$((FAIL + 1))
fi

# Check 5: clean shutdown of survivors
LINQU_BASE="$BASE" "$ORCH" --command shutdown 2>/dev/null
sleep 3
STILL_ALIVE=0
for i in $(seq 0 $((NUM_HOSTS - 1))); do
    [[ $i -eq 7 ]] && continue
    kill -0 "${DAEMON_PIDS[$i]}" 2>/dev/null && STILL_ALIVE=$((STILL_ALIVE + 1))
done
if [[ $STILL_ALIVE -eq 0 ]]; then
    echo "  [PASS] Check 5: all survivors shut down cleanly"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 5: $STILL_ALIVE survivors still running"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: ${PASS} PASS, ${FAIL} FAIL ==="
[[ $FAIL -eq 0 ]] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL
