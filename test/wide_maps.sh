#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-wide-map.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking typed-width Map value and structural key storage"
./build/beansc run examples/wide_maps.b >"$tmp/interp"
./build/beansc build examples/wide_maps.b -o "$tmp/native" >"$tmp/build" 2>&1
BEANS_NO_POOL=1 "$tmp/native" >"$tmp/native.out"
diff -u test/cases/wide_map.out "$tmp/interp"
diff -u test/cases/wide_map.out "$tmp/native.out"
grep -Eq 'call ptr @beans_map_new_typed_value\(i64 0, i64 24, i64 [1-9]' build/wide_maps.ll
grep -q 'call ptr @beans_map_new_typed_value(i64 0, i64 16, i64 0' build/wide_maps.ll
grep -q 'call void @beans_map_set_typed_raw' build/wide_maps.ll
grep -q 'call i64 @beans_map_get_typed_raw' build/wide_maps.ll
grep -q 'call i64 @beans_map_insert_typed_raw' build/wide_maps.ll
grep -q 'define internal i64 @bweq' build/wide_maps.ll
grep -q 'define internal i64 @bwhash' build/wide_maps.ll
grep -q 'call ptr @beans_map_keys_typed' build/wide_maps.ll

if ./build/beansc check test/cases/wide_map_key_bad.b >"$tmp/bad" 2>&1; then
    echo "expected non-Hash wide Map key to fail" >&2
    exit 1
fi
grep -q 'Map key needs Eq' "$tmp/bad"
grep -q 'Map key needs Hash' "$tmp/bad"

echo "ok wide Map values/keys, ARC fields, clone, reserve, and ordered holes"
