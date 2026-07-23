#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-fs-source.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking Beans-written high-level file helpers"
mkdir "$tmp/interp" "$tmp/native" "$tmp/asan"
./build/beansc run test/cases/fs_source.b -- "$tmp/interp" >"$tmp/interp.out"
./build/beansc build test/cases/fs_source.b -o "$tmp/fs-native" >"$tmp/build"
"$tmp/fs-native" "$tmp/native" >"$tmp/native.out"

diff -u test/cases/fs_source.out "$tmp/interp.out"
diff -u test/cases/fs_source.out "$tmp/native.out"
grep -q 'define .*@b_fs.read_bytes' build/fs_source.ll
grep -q 'define .*@b_fs.read' build/fs_source.ll
grep -q 'define .*@b_fs.write_bytes' build/fs_source.ll
grep -q 'define .*@b_fs.copy' build/fs_source.ll
if grep -Eq 'beans_file_(read_all|read_all_b|write_all|append_all|write_all_b|append_all_b|copy)' \
    build/beans_rt.c; then
    echo "migrated file helpers still exist in the native runtime" >&2
    exit 1
fi

clang -O1 -g -pthread -fsanitize=address -Wno-override-module \
    build/fs_source.ll build/beans_rt.c -o "$tmp/fs-asan"
BEANS_NO_POOL=1 "$tmp/fs-asan" "$tmp/asan" \
    >"$tmp/asan.out" 2>"$tmp/asan.err"
if grep -q 'AddressSanitizer' "$tmp/asan.err"; then
    cat "$tmp/asan.err" >&2
    exit 1
fi
diff -u test/cases/fs_source.out "$tmp/asan.out"

echo "ok File.open/read_at/write_at primitives with Beans policy code"
