#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-fixed-array.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking fixed array value and ABI parity"
./build/beansc run examples/fixed_arrays.b >"$tmp/interp"
./build/beansc build examples/fixed_arrays.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/fixed_array.out "$tmp/interp"
diff -u test/cases/fixed_array.out "$tmp/native.out"
grep -q '\[4 x i32\]' build/fixed_arrays.ll

echo "checking fixed array bounds parity"
set +e
./build/beansc run test/cases/fixed_array_oob.b >"$tmp/oob.interp" 2>&1
interp_status=$?
./build/beansc build test/cases/fixed_array_oob.b -o "$tmp/oob.native" \
    >"$tmp/oob.build" 2>&1
build_status=$?
if [[ "$build_status" -eq 0 ]]; then
    "$tmp/oob.native" >"$tmp/oob.native.out" 2>&1
    native_status=$?
else
    native_status=0
fi
set -e
if [[ "$interp_status" -eq 0 || "$build_status" -ne 0 || "$native_status" -eq 0 ]]; then
    echo "fixed array bounds test did not panic in both backends" >&2
    exit 1
fi
diff -u "$tmp/oob.interp" "$tmp/oob.native.out"
grep -q 'array index 2 out of range (len 2)' "$tmp/oob.interp"

echo "checking fixed array compile failures"
if ./build/beansc check test/cases/fixed_array_bad.b >"$tmp/bad" 2>&1; then
    echo "fixed_array_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q 'fixed array literal needs 3 element(s), got 2' "$tmp/bad"
grep -q 'fixed array length must be between 1 and 4096' "$tmp/bad"
grep -q 'fixed arrays need inline scalar, RawPtr, fixed-array, or struct elements' "$tmp/bad"
grep -q "'frozen' is a let — its elements can't be reassigned" "$tmp/bad"

echo "ok inline fixed arrays, copy semantics, iteration, bounds, and native ABI"
