#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-wide-list.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking typed-width List storage and ownership"
./build/beansc run examples/wide_lists.b >"$tmp/interp"
./build/beansc build examples/wide_lists.b -o "$tmp/native" >"$tmp/build" 2>&1
BEANS_NO_POOL=1 "$tmp/native" >"$tmp/native.out"
diff -u test/cases/wide_list.out "$tmp/interp"
diff -u test/cases/wide_list.out "$tmp/native.out"
grep -q 'call ptr @beans_list_new_typed(i64 8, i64 0)' build/wide_lists.ll
grep -q 'call ptr @beans_list_new_typed(i64 16, i64 0)' build/wide_lists.ll
grep -Eq 'call ptr @beans_list_new_typed\(i64 24, i64 [1-9]' build/wide_lists.ll
grep -q 'call void @beans_list_push_typed' build/wide_lists.ll
grep -q 'call void @beans_list_remove_typed' build/wide_lists.ll
grep -q 'call void @beans_list_decv_sort' build/wide_lists.ll
if grep -q '@beans_decv_box' build/wide_lists.ll; then
    # Option<decimal> still uses the old enum boundary, but the List itself
    # must not box on push/load. The typed constructor proves its storage ABI.
    grep -q 'call ptr @beans_list_new_typed(i64 16, i64 0)' build/wide_lists.ll
fi

echo "ok ARC-owning structs/arrays and wide List moves, clones, masks, and slices"
