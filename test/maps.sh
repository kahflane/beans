#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-map-models.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking Map and OrderedMap against a linear model"
./build/beansc run test/cases/map_models.b >"$tmp/interp"
./build/beansc build test/cases/map_models.b -o "$tmp/native" >"$tmp/build"
"$tmp/native" >"$tmp/native.out"
diff -u "$tmp/interp" "$tmp/native.out"
grep -q '^map model 0 ' "$tmp/interp"

clang -O1 -g -pthread -fsanitize=address -Wno-override-module \
    build/map_models.ll build/beans_rt.c -o "$tmp/asan"
BEANS_NO_POOL=1 "$tmp/asan" >"$tmp/asan.out" 2>"$tmp/asan.err"
if grep -q 'AddressSanitizer' "$tmp/asan.err"; then
    cat "$tmp/asan.err" >&2
    exit 1
fi
diff -u "$tmp/interp" "$tmp/asan.out"

echo "ok unordered swap-removal and ordered insertion order"
