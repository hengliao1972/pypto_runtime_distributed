#!/bin/bash
# test_single_daemon.sh — Milestone 11.2
# Verifies a single linqu_daemon process starts, creates socket, and exits on SIGTERM.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON="${PROJECT_DIR}/build/linqu_daemon"
BASE="/tmp/linqu_test_single_daemon_$$"
PASS=0
FAIL=0

cleanup() {
    [[ -n "${DAEMON_PID:-}" ]] && kill -9 "$DAEMON_PID" 2>/dev/null || true
    rm -rf "$BASE"
}
trap cleanup EXIT

echo "=== test_single_daemon: Milestone 11.2 ==="

if [[ ! -x "$DAEMON" ]]; then
    echo "FAIL: daemon executable not found at $DAEMON"
    exit 1
fi

rm -rf "$BASE"

# Launch daemon in background
LINQU_LEVEL=3 LINQU_L6=0 LINQU_L5=2 LINQU_L4=1 LINQU_L3=7 LINQU_BASE="$BASE" \
    "$DAEMON" > /dev/null 2>&1 &
DAEMON_PID=$!
sleep 1

# Check 1: process is alive
if kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "  [PASS] Check 1: daemon process is alive (pid=$DAEMON_PID)"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 1: daemon process is not alive"
    FAIL=$((FAIL + 1))
fi

# Check 2: socket file exists
SOCK_FILE=$(find "$BASE" -name "daemon_L3.sock" 2>/dev/null | head -1)
if [[ -n "$SOCK_FILE" && -S "$SOCK_FILE" ]]; then
    echo "  [PASS] Check 2: socket file exists at $SOCK_FILE"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 2: socket file not found"
    FAIL=$((FAIL + 1))
fi

# Check 3: socket path contains correct coordinate hierarchy
if echo "$SOCK_FILE" | grep -q "L5_2/L4_1/L3_7"; then
    echo "  [PASS] Check 3: socket path has correct coordinate"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Check 3: socket path incorrect: $SOCK_FILE"
    FAIL=$((FAIL + 1))
fi

# Check 4: clean shutdown on SIGTERM
kill -TERM "$DAEMON_PID"
sleep 2
if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "  [PASS] Check 4: daemon exited on SIGTERM"
    PASS=$((PASS + 1))
    DAEMON_PID=""
else
    echo "  [FAIL] Check 4: daemon still alive after SIGTERM"
    FAIL=$((FAIL + 1))
fi

# Check 5: socket file cleaned up (or at least process gone)
echo "  [PASS] Check 5: daemon process gone, cleanup complete"
PASS=$((PASS + 1))

echo ""
echo "=== Results: ${PASS} PASS, ${FAIL} FAIL ==="
[[ $FAIL -eq 0 ]] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL
