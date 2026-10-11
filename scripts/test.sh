#!/bin/bash
set -e

SO_PATH="./libleakscope.so"
FAILED=0

echo "============================================="
echo "        LeakScope Test Suite Running         "
echo "============================================="

# Test 1: Leaky sample (must report 3 leaked blocks)
echo -n "Test 1: leaky sample... "
OUT=$(LD_PRELOAD="$SO_PATH" ./samples/leaky 2>&1)
if echo "$OUT" | grep -iq "3 leaked blocks" || echo "$OUT" | grep -iq "Leaked Blocks:[[:space:]]*3"; then
    echo "PASS"
else
    echo "FAIL (Expected 3 leaked blocks)"
    echo "$OUT"
    FAILED=$((FAILED + 1))
fi

# Test 2: Multithreading sample (must report exactly 1 leak and exit with 0)
echo -n "Test 2: multithreaded sample... "
OUT=$(LD_PRELOAD="$SO_PATH" ./samples/threads 2>&1)
if (echo "$OUT" | grep -iq "1 leaked blocks" || echo "$OUT" | grep -iq "Leaked Blocks:[[:space:]]*1" || echo "$OUT" | grep -iq "1 leak") && ! echo "$OUT" | grep -iq "crash"; then
    echo "PASS"
else
    echo "FAIL (Expected 1 leak and no crash)"
    echo "$OUT"
    FAILED=$((FAILED + 1))
fi

# Test 3: Double free and invalid free detection (must report both and exit normally)
echo -n "Test 3: double free & invalid free... "
set +e
OUT=$(LD_PRELOAD="$SO_PATH" ./samples/double_free 2>&1)
EXIT_CODE=$?
set -e
if [ $EXIT_CODE -eq 0 ] && echo "$OUT" | grep -q "DOUBLE FREE" && echo "$OUT" | grep -q "INVALID FREE"; then
    echo "PASS"
else
    echo "FAIL (Expected DOUBLE FREE and INVALID FREE detection with clean exit)"
    echo "$OUT"
    FAILED=$((FAILED + 1))
fi

# Test 4: Clean sample (must report 0 leaks)
echo -n "Test 4: clean sample... "
OUT=$(LD_PRELOAD="$SO_PATH" ./samples/clean 2>&1)
if echo "$OUT" | grep -iq "0 leaks" || echo "$OUT" | grep -iq "Leaked Blocks:[[:space:]]*0"; then
    echo "PASS"
else
    echo "FAIL (Expected 0 leaks)"
    echo "$OUT"
    FAILED=$((FAILED + 1))
fi

echo "============================================="
if [ $FAILED -eq 0 ]; then
    echo "ALL TESTS PASSED!"
    exit 0
else
    echo "$FAILED TEST(S) FAILED!"
    exit 1
fi
