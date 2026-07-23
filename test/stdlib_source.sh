#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-stdlib-source.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking compiler-shipped Beans std packages"
./build/beansc run examples/stdlib_beans.b >"$tmp/interp"
./build/beansc build examples/stdlib_beans.b -o "$tmp/native" >"$tmp/build"
"$tmp/native" >"$tmp/native.out"

diff -u test/cases/stdlib_beans.out "$tmp/interp"
diff -u test/cases/stdlib_beans.out "$tmp/native.out"
grep -q 'define ptr @b_path.join' build/stdlib_beans.ll
grep -q 'define ptr @b_fmt.hex' build/stdlib_beans.ll
grep -q 'define ptr @b_fmt.bin' build/stdlib_beans.ll
grep -q 'define ptr @b_fmt.group' build/stdlib_beans.ll
grep -q 'define i64 @b_collections.increment' build/stdlib_beans.ll
grep -q 'define ptr @b_collections.map_values' build/stdlib_beans.ll
grep -q 'define internal i64 @bweq' build/stdlib_beans.ll
if grep -Eq 'beans_path_|beans_fmt_(hex|bin|group)' build/beans_rt.c; then
    echo "migrated path/fmt code still exists in the native runtime" >&2
    exit 1
fi

./build/beansc build bench/bytes.b -o "$tmp/bytes" >"$tmp/bytes-build"
grep -q 'define void @b_bytes.append_varint' build/bytes.ll
grep -q 'define i64 @b_bytes.decode_varint_at_or' build/bytes.ll
grep -q 'define i32 @b_bytes.crc32' build/bytes.ll
if grep -Eq 'call .*@beans_bytes_(append_varint|get_varint|crc32)' build/bytes.ll; then
    echo "bytes benchmark still calls migrated native algorithms" >&2
    exit 1
fi

echo "ok Beans collection policies, option/result, math, bytes, path, and fmt packages"
