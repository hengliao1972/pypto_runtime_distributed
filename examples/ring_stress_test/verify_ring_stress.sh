#!/bin/bash
# verify_ring_stress.sh — Verify ring stress test results
# Checks that scope enters == exits and allocation counts match expected.
set -euo pipefail

OUTPUT="${LINQU_RING_OUTPUT:-/tmp/linqu_ring_output}"
PASS=0
FAIL=0

echo "=== Ring Stress Verification ==="
echo "  Output dir: $OUTPUT"

if [[ ! -d "$OUTPUT" ]]; then
    echo "  SKIP: Output directory does not exist (run ring stress test first)"
    exit 0
fi

COUNT=$(find "$OUTPUT" -name "ring_result.json" 2>/dev/null | wc -l)
echo "  Found $COUNT ring_result.json files"

if [[ "$COUNT" -eq 0 ]]; then
    echo "  FAIL: No ring_result.json files found"
    exit 1
fi

ERRORS=0
while IFS= read -r f; do
    # Check balanced scopes
    balanced=$(grep -o '"balanced": [a-z]*' "$f" | head -1 | awk '{print $2}')
    if [[ "${balanced}" != "true" ]]; then
        echo "  FAIL: $f unbalanced scopes"
        ERRORS=$((ERRORS + 1))
        continue
    fi

    # Verify expected counts: 10 iterations * 3 depths * 4 tensors = 120 allocs
    allocs=$(grep -o '"total_allocs": [0-9]*' "$f" | head -1 | awk '{print $2}')
    if [[ "${allocs}" != "120" ]]; then
        echo "  FAIL: $f total_allocs=$allocs expected=120"
        ERRORS=$((ERRORS + 1))
    fi

    # 10 * 3 * 2 = 60 early frees
    frees=$(grep -o '"total_frees": [0-9]*' "$f" | head -1 | awk '{print $2}')
    if [[ "${frees}" != "60" ]]; then
        echo "  FAIL: $f total_frees=$frees expected=60"
        ERRORS=$((ERRORS + 1))
    fi

    # 10*(1+3) = 40 scope enters, 40 exits
    enters=$(grep -o '"scope_enters": [0-9]*' "$f" | head -1 | awk '{print $2}')
    if [[ "${enters}" != "40" ]]; then
        echo "  FAIL: $f scope_enters=$enters expected=40"
        ERRORS=$((ERRORS + 1))
    fi
done < <(find "$OUTPUT" -name "ring_result.json")

echo "  Checked $COUNT files, $ERRORS errors"
if [[ "$ERRORS" -eq 0 ]]; then
    echo "  PASS: All ring stress results valid"
else
    echo "  FAIL: $ERRORS validation errors"
    exit 1
fi
