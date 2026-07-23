#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-traits.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

./build/beansc run test/cases/traits_ok.b >"$tmp/interp"
./build/beansc build test/cases/traits_ok.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/traits_ok.out "$tmp/interp"
diff -u test/cases/traits_ok.out "$tmp/native.out"

if ./build/beansc check test/cases/traits_bad.b >"$tmp/bad" 2>&1; then
    echo "traits_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q "unknown trait 'Magic'" "$tmp/bad"
grep -q "List<T> has no method 'clone'" "$tmp/bad"
grep -q "needs T: Order, got Local" "$tmp/bad"
grep -q "cannot capture 'local' of non-Send type Local" "$tmp/bad"
grep -q "cannot capture 'shared' of non-Send type Shared<Local>" "$tmp/bad"
grep -q "closure returns non-Send type Local" "$tmp/bad"
grep -q "Map key needs Eq, got Shared<int>" "$tmp/bad"
grep -q "Map key needs Hash, got Shared<int>" "$tmp/bad"

echo "ok Clone/Eq/Hash/Order bounds and Send capture checks"
