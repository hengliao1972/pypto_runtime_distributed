#!/bin/bash
# run_test.sh — Run all example verification tests
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "============================================"
echo "  Linqu Distributed Runtime — Test Suite"
echo "============================================"
echo ""

PASS=0
FAIL=0

run_verify() {
    local name="$1"
    local script="$2"
    echo "--- Running: $name ---"
    if bash "$script"; then
        echo "  => $name: PASSED"
        PASS=$((PASS + 1))
    else
        echo "  => $name: FAILED"
        FAIL=$((FAIL + 1))
    fi
    echo ""
}

run_verify "Topology Test" "$SCRIPT_DIR/topology_test/verify_topology.sh"
run_verify "DAG Test"      "$SCRIPT_DIR/tensor_dag_test/verify_dag.sh"
run_verify "Ring Stress"   "$SCRIPT_DIR/ring_stress_test/verify_ring_stress.sh"

echo "============================================"
echo "  Results: $PASS PASSED, $FAIL FAILED"
echo "============================================"
exit $FAIL
