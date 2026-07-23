#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-c-abi-layout.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "checking C layouts, by-value records, pointer calls, and mixed floats"
if [[ "$(uname -s)" == "Darwin" ]]; then
    clang -O2 -dynamiclib test/fixtures/c_layout_helper.c -o "$tmp/layout.dylib"
    DYLD_INSERT_LIBRARIES="$tmp/layout.dylib" \
        ./build/beansc run test/cases/c_layout_c_abi.b >"$tmp/interp"
else
    clang -O2 -shared -fPIC test/fixtures/c_layout_helper.c -o "$tmp/layout.so"
    LD_PRELOAD="$tmp/layout.so" \
        ./build/beansc run test/cases/c_layout_c_abi.b >"$tmp/interp"
fi

# beansc still emits the checked IR/runtime before its normal link reports the
# intentionally external test symbols. Link the fixture into the acceptance
# binary explicitly.
./build/beansc build test/cases/c_layout_c_abi.b -o "$tmp/unlinked" \
    >"$tmp/generate" 2>&1 || true
test -f build/c_layout_c_abi.ll
test -f build/c_layout_c_abi_ffi.c
grep -q 'declare void @beans_ffi_wrap_' build/c_layout_c_abi.ll
grep -q 'beans_test_frame_roundtrip' build/c_layout_c_abi_ffi.c
clang -O2 -pthread -Wno-override-module build/c_layout_c_abi.ll \
    build/beans_rt.c build/c_layout_c_abi_ffi.c \
    test/fixtures/c_layout_helper.c -o "$tmp/native"
"$tmp/native" >"$tmp/native.out"

clang -O1 -g -pthread -fsanitize=address -Wno-override-module \
    build/c_layout_c_abi.ll build/beans_rt.c build/c_layout_c_abi_ffi.c \
    test/fixtures/c_layout_helper.c -o "$tmp/asan"
BEANS_NO_POOL=1 "$tmp/asan" >"$tmp/asan.out" 2>"$tmp/asan.err"
if grep -q 'AddressSanitizer' "$tmp/asan.err"; then
    cat "$tmp/asan.err" >&2
    exit 1
fi

diff -u test/cases/c_layout_c_abi.out "$tmp/interp"
diff -u test/cases/c_layout_c_abi.out "$tmp/native.out"
diff -u test/cases/c_layout_c_abi.out "$tmp/asan.out"

echo "ok host C nested layout, by-value struct/union ABI, pointers, and mixed floats"
