#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-moves.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

./build/beansc run test/cases/move_ok.b >"$tmp/interp"
./build/beansc build test/cases/move_ok.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/move_ok.out "$tmp/interp"
diff -u test/cases/move_ok.out "$tmp/native.out"
grep -q "define void @b_swap(ptr %p0, ptr %p1)" build/move_ok.ll
grep -q "call void @b_replace(ptr" build/move_ok.ll

if ./build/beansc check test/cases/move_bad.b >"$tmp/bad" 2>&1; then
    echo "move_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q "use of moved value 'item'" "$tmp/bad"
grep -q "value 'item' may have been moved" "$tmp/bad"
grep -q "can't take borrowed binding 'item'" "$tmp/bad"
grep -q "can't take outer value 'item' from a loop or escaping closure" "$tmp/bad"
grep -q "take needs a local name" "$tmp/bad"

if ./build/beansc check test/cases/box_move_bad.b >"$tmp/box-bad" 2>&1; then
    echo "box_move_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q "because Box<int> is move-only" "$tmp/box-bad"
grep -q "List.push needs 'take second'" "$tmp/box-bad"
grep -q "some needs 'take second'" "$tmp/box-bad"
grep -q "Shared.new needs 'take second'" "$tmp/box-bad"

if ./build/beansc check test/cases/arena_move_bad.b >"$tmp/arena-bad" 2>&1; then
    echo "arena_move_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q "because Arena<int> is move-only" "$tmp/arena-bad"

if ./build/beansc check test/cases/collection_move_bad.b >"$tmp/collection-bad" 2>&1; then
    echo "collection_move_bad.b unexpectedly passed" >&2
    exit 1
fi
grep -q "return needs 'take values' because List<int> is move-only" "$tmp/collection-bad"
grep -q "binding 'copied' needs 'take values' because List<int> is move-only" "$tmp/collection-bad"
grep -q "binding 'copied_map' needs 'take map' because Map<string, int> is move-only" "$tmp/collection-bad"
grep -q "List<List<int>> has no method 'clone'" "$tmp/collection-bad"
grep -q "consume.*take argument 1 needs 'take values'" "$tmp/collection-bad"
grep -q "has ownership parameters and cannot be stored as a value yet" "$tmp/collection-bad"
grep -q "because Packet is move-only" "$tmp/collection-bad"
grep -q "inout argument 1 must be 'inout var_name'" "$tmp/collection-bad"
grep -q "inout needs var, but 'fixed' is a let" "$tmp/collection-bad"
grep -q "overlapping inout arguments for 'left'" "$tmp/collection-bad"
grep -q "inout is only valid for an inout call argument" "$tmp/collection-bad"
grep -q "closure cannot capture inout parameter 'value'" "$tmp/collection-bad"
grep -q "changes ownership mode of argument 1" "$tmp/collection-bad"

echo "ok take, move-only buffers/handles, explicit clones, branches, and use-after-move errors"
