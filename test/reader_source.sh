#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-reader-source.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking Beans-written buffered reader"
mkdir "$tmp/interp" "$tmp/native" "$tmp/asan"
./build/beansc run test/cases/reader_source.b -- "$tmp/interp" >"$tmp/interp.out"
./build/beansc build test/cases/reader_source.b -o "$tmp/reader-native" >"$tmp/build"
"$tmp/reader-native" "$tmp/native" >"$tmp/native.out"

diff -u test/cases/reader_source.out "$tmp/interp.out"
diff -u test/cases/reader_source.out "$tmp/native.out"
grep -q 'define .*@m_reader.Reader_read_line' build/reader_source.ll
if grep -q 'beans_bufr_' build/beans_rt.c; then
    echo "migrated buffered reader still exists in the native runtime" >&2
    exit 1
fi

clang -O1 -g -pthread -fsanitize=address -Wno-override-module \
    build/reader_source.ll build/beans_rt.c -o "$tmp/reader-asan"
BEANS_NO_POOL=1 "$tmp/reader-asan" "$tmp/asan" \
    >"$tmp/asan.out" 2>"$tmp/asan.err"
if grep -q 'AddressSanitizer' "$tmp/asan.err"; then
    cat "$tmp/asan.err" >&2
    exit 1
fi
diff -u test/cases/reader_source.out "$tmp/asan.out"

echo "ok File.read_at primitive with Beans buffering and line policy"
