#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-wide-concurrency.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking typed-width Channel and Thread storage"
./build/beansc run examples/wide_concurrency.b >"$tmp/interp"
./build/beansc build examples/wide_concurrency.b -o "$tmp/native" >"$tmp/build" 2>&1
BEANS_NO_POOL=1 "$tmp/native" >"$tmp/native.out"
diff -u test/cases/wide_concurrency.out "$tmp/interp"
diff -u test/cases/wide_concurrency.out "$tmp/native.out"
grep -Eq 'call ptr @beans_chan_new_typed\(i64 1, i64 16, i64 1\)' build/wide_concurrency.ll
grep -q 'call i64 @beans_chan_send_typed' build/wide_concurrency.ll
grep -q 'call i64 @beans_chan_recv_typed' build/wide_concurrency.ll
grep -q 'channel.or.ok' build/wide_concurrency.ll
grep -q 'call ptr @beans_thread_spawn_typed' build/wide_concurrency.ll
grep -q 'call void @beans_thread_join_typed' build/wide_concurrency.ll
grep -Fq 'define void @b_send_one$Event' build/wide_concurrency.ll

echo "ok wide queues/results, generic specialization, ARC transfer, and cycles"
