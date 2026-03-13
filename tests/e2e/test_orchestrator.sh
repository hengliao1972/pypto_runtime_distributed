#!/bin/bash
# test_orchestrator.sh — Milestone 12.3
# Verifies linqu_orchestrator can discover and shut down daemons.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON="${PROJECT_DIR}/build/linqu_daemon"
ORCH="${PROJECT_DIR}/build/linqu_orchestrator"
BASE="/tmp/linqu_test_orch_$$"
PASS=0
FAIL=0

cleanup() {
    kill $(jobs -p) 2>/dev/null || true
    sleep 1
    rm -rf "$BASE"
}
trap cleanup EXIT

echo "=== test_orchestrator: Milestone 12.3 ==="

rm -rf "$BASE"

# Launch 4 L3 daemons (1 pod: l5=0, l4=0, l3=0..3)
for l3 in 0 1 2 3; do
    LINQU_LEVEL=3 LINQU_L6=0 LINQU_L5=0 LINQU_L4=0 LINQU_L3=$l3 LINQU_BASE="$BASE" \
        "$DAEMON" > /dev/null 2>&1 &
done
sleep 2

# Check 1: orchestrator discovers all 4 daemons
OUTPUT=$(LINQU_BASE="$BASE" "$ORCH" --command discover 2>/dev/null)
# Use python-style extraction to be robust
L3_COUNT=$(echo "$OUTPUT" | python3 -c "import sys,json; print(json.load(sys.stdin)['l3_hosts'])" 2>/dev/null || echo "$OUTPUT" | grep -oP '"l3_hosts":\s*\K\d+')
if [[ "$L3_COUNT" == "4" ]]; then
    echo "  [PASS] Check 1: discovered 4 L3 hosts"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 1: expected 4 L3 hosts, got '$L3_COUNT'"
    echo "$OUTPUT"
    FAIL=$((FAIL + 1))
fi

# Check 2: status shows healthy
STATUS=$(echo "$OUTPUT" | python3 -c "import sys,json; print(json.load(sys.stdin)['status'])" 2>/dev/null || echo "unknown")
if [[ "$STATUS" == "healthy" ]]; then
    echo "  [PASS] Check 2: cluster status is healthy"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 2: status is '$STATUS'"
    FAIL=$((FAIL + 1))
fi

# Check 3: orchestrator can send shutdown to all daemons
LINQU_BASE="$BASE" "$ORCH" --command shutdown 2>&1
sleep 3

# Verify daemons have exited
ALIVE=0
for pid in $(jobs -p 2>/dev/null); do
    if kill -0 "$pid" 2>/dev/null; then
        ALIVE=$((ALIVE + 1))
    fi
done

if [[ $ALIVE -eq 0 ]]; then
    echo "  [PASS] Check 3: all daemons shut down after orchestrator shutdown"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 3: $ALIVE daemons still alive after shutdown (killing manually)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: ${PASS} PASS, ${FAIL} FAIL ==="
[[ $FAIL -eq 0 ]] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL
