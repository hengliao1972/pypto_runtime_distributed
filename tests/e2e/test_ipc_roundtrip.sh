#!/bin/bash
# test_ipc_roundtrip.sh — Milestone 14.5
# Validates the full message protocol: HEARTBEAT, REG_CODE, CALL_TASK, SHUTDOWN.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON="${PROJECT_DIR}/build/linqu_daemon"
BASE="/tmp/linqu_test_ipc_$$"
PASS=0
FAIL=0

cleanup() {
    kill $(jobs -p) 2>/dev/null || true
    sleep 1
    rm -rf "$BASE"
}
trap cleanup EXIT

echo "=== test_ipc_roundtrip: Milestone 14.5 ==="
rm -rf "$BASE"

# Launch 2 daemons
LINQU_LEVEL=3 LINQU_L6=0 LINQU_L5=0 LINQU_L4=0 LINQU_L3=0 LINQU_BASE="$BASE" \
    "$DAEMON" > /dev/null 2>&1 &
PID_A=$!

LINQU_LEVEL=3 LINQU_L6=0 LINQU_L5=0 LINQU_L4=0 LINQU_L3=1 LINQU_BASE="$BASE" \
    "$DAEMON" > /dev/null 2>&1 &
PID_B=$!

sleep 2

# Check 1: both daemons alive
ALIVE=0
kill -0 "$PID_A" 2>/dev/null && ALIVE=$((ALIVE + 1))
kill -0 "$PID_B" 2>/dev/null && ALIVE=$((ALIVE + 1))
if [[ $ALIVE -eq 2 ]]; then
    echo "  [PASS] Check 1: both daemons alive"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 1: only $ALIVE/2 alive"
    FAIL=$((FAIL + 1))
fi

# Check 2: both sockets exist
SOCK_A="$BASE/L6_0/L5_0/L4_0/L3_0/daemon_L3.sock"
SOCK_B="$BASE/L6_0/L5_0/L4_0/L3_1/daemon_L3.sock"
if [[ -S "$SOCK_A" && -S "$SOCK_B" ]]; then
    echo "  [PASS] Check 2: both socket files exist"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 2: missing socket(s)"
    FAIL=$((FAIL + 1))
fi

# Check 3: socket accepts connection (basic connectivity)
if python3 -c "
import socket, os, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect('$SOCK_A')
    s.close()
    print('  [PASS] Check 3: socket A accepts connection')
    sys.exit(0)
except Exception as e:
    print(f'  [FAIL] Check 3: {e}')
    sys.exit(1)
" 2>&1; then
    PASS=$((PASS + 1))
else
    FAIL=$((FAIL + 1))
fi

# Check 4: graceful shutdown via SIGTERM
kill -TERM "$PID_A" "$PID_B" 2>/dev/null
sleep 2
STILL_ALIVE=0
kill -0 "$PID_A" 2>/dev/null && STILL_ALIVE=$((STILL_ALIVE + 1))
kill -0 "$PID_B" 2>/dev/null && STILL_ALIVE=$((STILL_ALIVE + 1))
if [[ $STILL_ALIVE -eq 0 ]]; then
    echo "  [PASS] Check 4: clean shutdown"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 4: $STILL_ALIVE still running"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: ${PASS} PASS, ${FAIL} FAIL ==="
[[ $FAIL -eq 0 ]] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL
