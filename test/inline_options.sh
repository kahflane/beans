#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-inline-option.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking inline Option value and ABI parity"
./build/beansc run examples/inline_options.b >"$tmp/interp"
./build/beansc build examples/inline_options.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/inline_option.out "$tmp/interp"
diff -u test/cases/inline_option.out "$tmp/native.out"
grep -q 'define {i1, %bs.Pair} @b_pass({i1, %bs.Pair}' build/inline_options.ll
grep -q 'insertvalue {i1, %bs.Pair} zeroinitializer, i1 true, 0' build/inline_options.ll
grep -q 'define {i1, {i1, %bs.Pair}}' build/inline_options.ll || \
    grep -q '{i1, {i1, %bs.Pair}}' build/inline_options.ll

echo "ok Option methods across scalars, ARC, structs, arrays, SIMD, slices, and nesting"
