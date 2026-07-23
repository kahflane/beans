#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "checking parser error recovery on half-typed code"

out=$(./build/beansc parse test/cases/recover.b 2>&1 || true)

# the incomplete member access is reported
grep -q "expected name after '.'" <<<"$out" ||
    { echo "FAIL: missing the member-access error" >&2; echo "$out" >&2; exit 1; }
# ...but the receiver is still in the tree (for completion)
grep -q "u\." <<<"$out" ||
    { echo "FAIL: receiver 'u.' was dropped from the AST" >&2; echo "$out" >&2; exit 1; }
# ...and the following statement survived rather than being devoured
grep -q "let z: int = 5" <<<"$out" ||
    { echo "FAIL: 'let z' did not survive recovery" >&2; echo "$out" >&2; exit 1; }

echo "ok parser recovery: error reported, receiver kept, next statement survives"
