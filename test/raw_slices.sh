#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-raw-slice.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking raw slice value and ABI parity"
./build/beansc run examples/raw_slices.b >"$tmp/interp"
./build/beansc build examples/raw_slices.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/raw_slice.out "$tmp/interp"
diff -u test/cases/raw_slice.out "$tmp/native.out"
grep -q 'insertvalue {ptr, i64}' build/raw_slices.ll

echo "checking raw slice bounds parity"
set +e
./build/beansc run test/cases/raw_slice_oob.b >"$tmp/oob.interp" 2>&1
interp_status=$?
./build/beansc build test/cases/raw_slice_oob.b -o "$tmp/oob.native" \
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
    echo "raw slice bounds test did not panic in both backends" >&2
    exit 1
fi
diff -u "$tmp/oob.interp" "$tmp/oob.native.out"
grep -q 'slice index 2 out of range (len 2)' "$tmp/oob.interp"

echo "checking raw slice compile failures"
if ./build/beansc check test/cases/raw_slice_bad.b >"$tmp/bad" 2>&1; then
    echo "raw_slice_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q 'Slice.from_raw requires unsafe' "$tmp/bad"
grep -q 'Slice.set requires unsafe' "$tmp/bad"
grep -q 'Slice.subslice requires unsafe' "$tmp/bad"
grep -q 'looping over Slice requires unsafe' "$tmp/bad"
grep -q 'Slice indexing requires unsafe' "$tmp/bad"
grep -q 'Slice only supports inline scalars, RawPtr, fixed arrays, and extern "C" struct/union values' "$tmp/bad"

echo "ok two-word raw slices, checked access, subviews, iteration, and ABI"
