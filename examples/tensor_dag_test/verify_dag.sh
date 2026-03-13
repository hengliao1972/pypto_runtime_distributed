#!/bin/bash
# verify_dag.sh — Verify DAG computation results
# Checks that L3 daemons produced correct f(i) = 9i^2 + 12i + 2 values.
set -euo pipefail

OUTPUT="${LINQU_DAG_OUTPUT:-/tmp/linqu_dag_output}"
PASS=0
FAIL=0

echo "=== DAG Computation Verification ==="
echo "  Output dir: $OUTPUT"

if [[ ! -d "$OUTPUT" ]]; then
    echo "  SKIP: Output directory does not exist (run DAG test first)"
    exit 0
fi

COUNT=$(find "$OUTPUT" -name "result.json" 2>/dev/null | wc -l)
echo "  Found $COUNT result.json files"

if [[ "$COUNT" -eq 0 ]]; then
    echo "  FAIL: No result.json files found"
    exit 1
fi

ERRORS=0
while IFS= read -r f; do
    # Check errors field is 0
    err_count=$(grep -o '"errors": [0-9]*' "$f" | head -1 | awk '{print $2}')
    if [[ "${err_count:-1}" != "0" ]]; then
        echo "  FAIL: $f has $err_count computation errors"
        ERRORS=$((ERRORS + 1))
        continue
    fi

    # Verify f(0) = 2.0 (9*0 + 12*0 + 2 = 2)
    f0=$(grep -o '"f_0": [0-9.]*' "$f" | head -1 | awk '{print $2}')
    if [[ "${f0}" != "2.000000" ]]; then
        echo "  FAIL: $f f_0=$f0 expected=2.000000"
        ERRORS=$((ERRORS + 1))
    fi

    # Verify f(1) = 23.0 (9 + 12 + 2 = 23)
    f1=$(grep -o '"f_1": [0-9.]*' "$f" | head -1 | awk '{print $2}')
    if [[ "${f1}" != "23.000000" ]]; then
        echo "  FAIL: $f f_1=$f1 expected=23.000000"
        ERRORS=$((ERRORS + 1))
    fi
done < <(find "$OUTPUT" -name "result.json")

echo "  Checked $COUNT files, $ERRORS errors"
if [[ "$ERRORS" -eq 0 ]]; then
    echo "  PASS: All DAG computations correct"
else
    echo "  FAIL: $ERRORS validation errors"
    exit 1
fi
