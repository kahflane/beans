#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-c-layout.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking inline C-layout struct parity"
./build/beansc run examples/c_layout_structs.b >"$tmp/interp"
./build/beansc build examples/c_layout_structs.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/c_layout_struct.out "$tmp/interp"
diff -u test/cases/c_layout_struct.out "$tmp/native.out"
grep -q '%bs.Packet = type {i8, i32, float, i1}' build/c_layout_structs.ll
grep -q '%bs.Link = type {i32, ptr}' build/c_layout_structs.ll
grep -q '%bs.Pair = type {i16, \[3 x i8\]}' build/c_layout_structs.ll
grep -q '%bs.Frame = type {%bs.Pair, \[2 x i32\], \[2 x ptr\]}' build/c_layout_structs.ll
grep -q 'define %bs.Packet @b_bumped(%bs.Packet' build/c_layout_structs.ll

echo "checking struct compile failures"
if ./build/beansc check test/cases/c_layout_attribute_bad.b >"$tmp/attribute" 2>&1; then
    echo "c_layout_attribute_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q '@c_layout applies to structs/unions, not classes' "$tmp/attribute"
if ./build/beansc check test/cases/c_layout_struct_bad.b >"$tmp/bad" 2>&1; then
    echo "c_layout_struct_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q 'structs need at least one field' "$tmp/bad"
grep -q 'struct/union fields need inline scalar, RawPtr, fixed-array, or nested struct storage' "$tmp/bad"
grep -q "recursive inline layout through field 'next' has no finite size" "$tmp/bad"
grep -q 'generic structs are not available yet' "$tmp/bad"
grep -q 'structs cannot inherit' "$tmp/bad"
grep -q 'struct methods are not available yet' "$tmp/bad"
grep -q "is a let — its fields can't be reassigned" "$tmp/bad"
grep -q 'RawPtr only supports inline scalars, RawPtr, fixed arrays, and @c_layout struct/union values' "$tmp/bad"

echo "ok inline struct copies, target layout, raw memory, slices, and native ABI"
