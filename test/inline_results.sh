#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-inline-result.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking inline Result value, ownership, and ABI parity"
./build/beansc run examples/inline_results.b >"$tmp/interp"
./build/beansc build examples/inline_results.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/inline_result.out "$tmp/interp"
diff -u test/cases/inline_result.out "$tmp/native.out"
grep -q 'define {i1, %bs.Pair, ptr} @b_pass({i1, %bs.Pair, ptr}' build/inline_results.ll
grep -q 'insertvalue {i1, %bs.Pair, ptr} zeroinitializer, i1 false, 0' build/inline_results.ll
grep -q 'call void @beans_retain(ptr' build/inline_results.ll
grep -q 'call void @beans_release(ptr' build/inline_results.ll

echo "ok inline Result structs, ARC payloads, nesting, capture, match, try, and assignment"
