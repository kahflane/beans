#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-closure-captures.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking value and shared-cell closure captures"
./build/beansc run test/cases/closure_captures.b >"$tmp/interp"
./build/beansc build test/cases/closure_captures.b -o "$tmp/native" \
    >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/closure_captures.out "$tmp/interp"
diff -u test/cases/closure_captures.out "$tmp/native.out"

# The immutable scalar lives directly in the closure environment. Variables
# changed outside or inside a closure still use the shared heap-cell path.
grep -q '%cap0.v = load i64' build/closure_captures.ll
test "$(grep -c '%cap0.c = load ptr' build/closure_captures.ll)" -eq 2

echo "ok immutable captures use values and mutable captures use shared cells"
