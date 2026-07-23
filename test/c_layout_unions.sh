#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-c-union.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking inline C-layout union parity"
./build/beansc run examples/c_layout_unions.b >"$tmp/interp"
./build/beansc build examples/c_layout_unions.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/c_layout_union.out "$tmp/interp"
diff -u test/cases/c_layout_union.out "$tmp/native.out"
grep -q '%bs.Word = type {i32}' build/c_layout_unions.ll
grep -q '%bs.AlignedBlock = type {i64, \[8 x i8\]}' build/c_layout_unions.ll
grep -q 'define %bs.Word @b_passthrough(%bs.Word' build/c_layout_unions.ll

echo "checking union compile failures"
if ./build/beansc check test/cases/c_layout_union_attribute_bad.b >"$tmp/attribute" 2>&1; then
    echo "c_layout_union_attribute_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q 'union requires @c_layout' "$tmp/attribute"
if ./build/beansc check test/cases/c_layout_union_bad.b >"$tmp/bad" 2>&1; then
    echo "c_layout_union_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q 'unions need at least one field' "$tmp/bad"
grep -q 'struct/union fields need inline scalar, RawPtr, fixed-array, or nested struct storage' "$tmp/bad"
grep -q 'generic unions are not available yet' "$tmp/bad"
grep -q 'union fields cannot have defaults' "$tmp/bad"
grep -q 'union methods are not available yet' "$tmp/bad"
grep -q 'union initialization requires unsafe' "$tmp/bad"
grep -q 'union field access requires unsafe' "$tmp/bad"
grep -q 'union initializer sets exactly one field, got 0' "$tmp/bad"
grep -q 'union initializer sets exactly one field, got 2' "$tmp/bad"
grep -q 'union fields only support direct assignment for now' "$tmp/bad"

echo "ok overlapping union fields, unsafe access, raw memory, and inline ABI"
