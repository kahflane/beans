#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-wide-owner.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking typed-width Box and Arena storage"
./build/beansc run examples/wide_owners.b >"$tmp/interp"
./build/beansc build examples/wide_owners.b -o "$tmp/native" >"$tmp/build" 2>&1
BEANS_NO_POOL=1 "$tmp/native" >"$tmp/native.out"
diff -u test/cases/wide_owner.out "$tmp/interp"
diff -u test/cases/wide_owner.out "$tmp/native.out"
grep -Eq 'call ptr @beans_box_new_typed\(ptr .*i64 16, i64 1, i64 0\)' build/wide_owners.ll
grep -q 'call void @beans_box_get_typed' build/wide_owners.ll
grep -q 'call void @beans_box_set_typed' build/wide_owners.ll
grep -Eq 'call ptr @beans_arena_new_typed\(i64 1, i64 16, i64 1, i64 0' build/wide_owners.ll
grep -Eq 'call ptr @beans_arena_new_typed\(i64 1, i64 8, i64 1, i64 1' build/wide_owners.ll
grep -q 'call i64 @beans_arena_put_typed' build/wide_owners.ll
grep -q 'call i64 @beans_arena_get_typed' build/wide_owners.ll
grep -q 'call void @beans_arena_at_typed' build/wide_owners.ll
grep -Fq 'define ptr @b_boxed$Event' build/wide_owners.ll
grep -Fq 'define i64 @b_store_one$Event' build/wide_owners.ll

echo "checking typed Arena bounds parity"
set +e
./build/beansc run test/cases/wide_arena_oob.b >"$tmp/oob.interp" 2>&1
interp_status=$?
./build/beansc build test/cases/wide_arena_oob.b -o "$tmp/oob" \
    >"$tmp/oob.build" 2>&1
"$tmp/oob" >"$tmp/oob.native" 2>&1
native_status=$?
set -e
test "$interp_status" -eq 3
test "$native_status" -eq 3
diff -u "$tmp/oob.interp" "$tmp/oob.native"

echo "ok wide values, generic specialization, ARC copies, clear, and cycles"
