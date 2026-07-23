#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-numeric.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

./build/beansc run test/cases/numeric_ok.b >"$tmp/interp"
./build/beansc build test/cases/numeric_ok.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/numeric_ok.out "$tmp/interp"
diff -u test/cases/numeric_ok.out "$tmp/native.out"

# Decimal is a real 16-byte value in normal native code. The one-slot generic
# runtime boxes it only at a remaining one-slot key or enum boundary.
grep -q 'define i128 @b_add_cent(i128' build/numeric_ok.ll
grep -q 'store i128' build/numeric_ok.ll
grep -q '@beans_decv_box(i128' build/numeric_ok.ll

if ./build/beansc check test/cases/numeric_bad.b >"$tmp/bad" 2>&1; then
    echo "numeric_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q "128 does not fit i8 (-128..127)" "$tmp/bad"
grep -q -- "-129 does not fit i8 (-128..127)" "$tmp/bad"
grep -q -- "-1 does not fit u8 (0..255)" "$tmp/bad"
grep -q "256 does not fit u8 (0..255)" "$tmp/bad"
grep -q "2147483648 does not fit i32 (-2147483648..2147483647)" "$tmp/bad"
grep -q "18446744073709551616 does not fit u64 (0..18446744073709551615)" "$tmp/bad"
grep -q "32768 does not fit i16 (-32768..32767)" "$tmp/bad"
grep -q -- "-32769 does not fit i16 (-32768..32767)" "$tmp/bad"
grep -q "65536 does not fit u16 (0..65535)" "$tmp/bad"
grep -q -- "-2147483649 does not fit i32 (-2147483648..2147483647)" "$tmp/bad"
grep -q "4294967296 does not fit u32 (0..4294967295)" "$tmp/bad"
grep -q "9223372036854775808 does not fit int (-9223372036854775808..9223372036854775807)" "$tmp/bad"
grep -q -- "-9223372036854775809 does not fit int (-9223372036854775808..9223372036854775807)" "$tmp/bad"
grep -q -- "-1 does not fit u64 (0..18446744073709551615)" "$tmp/bad"

echo "ok numeric widths, boundaries, unsigned operations, f32, and inline decimal"
