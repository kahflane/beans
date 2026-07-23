#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-syntax-v07.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

./build/beansc run test/cases/syntax_v07_ok.b >"$tmp/interp"
./build/beansc build test/cases/syntax_v07_ok.b -o "$tmp/native" >"$tmp/build" 2>&1
"$tmp/native" >"$tmp/native.out"
diff -u test/cases/syntax_v07_ok.out "$tmp/interp"
diff -u test/cases/syntax_v07_ok.out "$tmp/native.out"

check_bad() {
    local file=$1
    local message=$2
    if ./build/beansc check "test/cases/$file" >"$tmp/bad" 2>&1; then
        echo "$file unexpectedly passed" >&2
        exit 1
    fi
    grep -q "$message" "$tmp/bad"
}

check_bad syntax_old_call_bad.b "classes are built with 'new Item(...)'"
check_bad syntax_raw_class_bad.b "field literals are only for structs"
check_bad syntax_dot_new_bad.b "use 'new Type(...)'"
check_bad syntax_static_bad.b "declare 'static fn make'"
check_bad syntax_static_bad.b "Child has no static 'answer'"
check_bad syntax_self_bad.b "self is implicit"
check_bad syntax_static_self_bad.b "self isn't available here"
check_bad syntax_unique_inherited_bad.b "needs 'move first'"
check_bad syntax_inheritance_bad.b "extends needs a class"
check_bad syntax_inheritance_bad.b "implements needs an interface"
check_bad syntax_inheritance_bad.b "interfaces may extend only interfaces"
check_bad syntax_interface_static_bad.b "static interface methods are not supported"
check_bad syntax_bound_bad.b "generic bound 'Value' is not an interface"
check_bad syntax_old_take_bad.b "'take' was removed"
check_bad syntax_old_forms_bad.b "@move_only was removed"
check_bad syntax_old_forms_bad.b "':' inheritance was removed"
check_bad syntax_old_forms_bad.b "':' generic bounds were removed"

echo "ok v0.7 construction, statics, interfaces, move, unique, and C layout syntax"
