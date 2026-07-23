#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-wide-sync.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking typed-width Shared and Mutex storage"
./build/beansc run examples/wide_sync.b >"$tmp/interp"
./build/beansc build examples/wide_sync.b -o "$tmp/native" >"$tmp/build" 2>&1
BEANS_NO_POOL=1 "$tmp/native" >"$tmp/native.out"
diff -u test/cases/wide_sync.out "$tmp/interp"
diff -u test/cases/wide_sync.out "$tmp/native.out"
grep -Eq 'call ptr @beans_shared_new_typed\(ptr .*i64 16, i64 1\)' build/wide_sync.ll
grep -q 'call void @beans_shared_get_typed' build/wide_sync.ll
grep -Eq 'call ptr @beans_mutex_new_typed\(ptr .*i64 16, i64 1\)' build/wide_sync.ll
grep -q 'call void @beans_mutex_lock_typed' build/wide_sync.ll
grep -q 'private unnamed_addr constant {i64, i64, ptr}' build/wide_sync.ll
grep -Fq 'define ptr @b_share$Event' build/wide_sync.ll
grep -Fq 'define ptr @b_guard$Event' build/wide_sync.ll

echo "ok wide values, weak expiry, generic specialization, threads, and cycles"
