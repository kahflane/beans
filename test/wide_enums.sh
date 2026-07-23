#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-wide-enum.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking typed-width user-enum payload storage"
./build/beansc run examples/wide_enums.b >"$tmp/interp"
./build/beansc build examples/wide_enums.b -o "$tmp/native" >"$tmp/build" 2>&1
BEANS_NO_POOL=1 "$tmp/native" >"$tmp/native.out"
diff -u test/cases/wide_enum.out "$tmp/interp"
diff -u test/cases/wide_enum.out "$tmp/native.out"
grep -q 'store %bs.Pair' build/wide_enums.ll
grep -q 'store \[2 x i64\]' build/wide_enums.ll
grep -q 'define internal i64 @beq' build/wide_enums.ll
grep -q 'define internal i64 @bweq' build/wide_enums.ll

echo "ok structs, arrays, ARC fields, mixed payloads, match, equality, and hash"
