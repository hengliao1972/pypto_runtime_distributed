#!/bin/bash
# verify_topology.sh — Verify topology test output
# Checks that L3 daemons wrote identity.json with correct coordinates.
set -euo pipefail

OUTPUT="${LINQU_TOPO_OUTPUT:-/tmp/linqu_topo_output}"
PASS=0
FAIL=0

echo "=== Topology Verification ==="
echo "  Output dir: $OUTPUT"

if [[ ! -d "$OUTPUT" ]]; then
    echo "  SKIP: Output directory does not exist (run topology test first)"
    exit 0
fi

# Count identity.json files
COUNT=$(find "$OUTPUT" -name "identity.json" 2>/dev/null | wc -l)
echo "  Found $COUNT identity.json files"

if [[ "$COUNT" -eq 0 ]]; then
    echo "  FAIL: No identity.json files found"
    exit 1
fi

# Verify each identity.json has valid JSON with coordinate fields
ERRORS=0
while IFS= read -r f; do
    dir=$(dirname "$f")
    basename=$(basename "$dir")

    # Extract expected L3 index from directory name
    expected_l3=$(echo "$basename" | sed 's/L3_//')

    # Check JSON has required fields
    if ! grep -q '"pid"' "$f" 2>/dev/null; then
        echo "  FAIL: $f missing pid field"
        ERRORS=$((ERRORS + 1))
        continue
    fi
    if ! grep -q '"coordinate"' "$f" 2>/dev/null; then
        echo "  FAIL: $f missing coordinate field"
        ERRORS=$((ERRORS + 1))
        continue
    fi

    # Verify L3 coordinate matches directory
    actual_l3=$(grep -o '"l3": [0-9]*' "$f" | head -1 | awk '{print $2}')
    if [[ "$actual_l3" != "$expected_l3" ]]; then
        echo "  FAIL: $f l3=$actual_l3 expected=$expected_l3"
        ERRORS=$((ERRORS + 1))
    fi
done < <(find "$OUTPUT" -name "identity.json")

echo "  Checked $COUNT files, $ERRORS errors"
if [[ "$ERRORS" -eq 0 ]]; then
    echo "  PASS: All topology identity files valid"
else
    echo "  FAIL: $ERRORS validation errors"
    exit 1
fi
