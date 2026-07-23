#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
bin=./build/beansc
fixture=test/cases/doc_hover.b

echo "checking lsp-probe hover output"

expect() { # <file:line:col> <substring that must appear>
    local pos="$1" want="$2" got
    got=$("$bin" lsp-probe "$pos")
    if ! grep -qF -- "$want" <<<"$got"; then
        echo "FAIL $pos: expected to contain: $want" >&2
        echo "--- got ---" >&2
        echo "$got" >&2
        exit 1
    fi
}

expect_none() { # <file:line:col> — must resolve to nothing (nonzero exit)
    local pos="$1"
    if "$bin" lsp-probe "$pos" >/dev/null 2>&1; then
        echo "FAIL $pos: expected no symbol (nonzero exit)" >&2
        exit 1
    fi
}

# function: signature + the rendered /// doc block, including the tag line
expect "$fixture:9:4"   'fn add(a: int, b: int) -> int'
expect "$fixture:9:4"   'Adds two integers and returns the sum.'
expect "$fixture:9:4"   'When to use:'
# type name
expect "$fixture:17:7"  'class Point'
expect "$fixture:17:7"  'A point in 2-D space.'
# field, qualified by its owner, with its own doc
expect "$fixture:19:5"  'Point.x: int'
expect "$fixture:19:5"  'The horizontal coordinate.'
# method, qualified, self made explicit
expect "$fixture:28:8"  'fn Point.norm2(self) -> int'
expect "$fixture:28:8"  'Distance from the origin, squared.'
# enum and a variant
expect "$fixture:34:6"  'enum Payment'
expect "$fixture:34:6"  'How a customer paid.'
expect "$fixture:36:5"  'Payment.cash'
expect "$fixture:36:5"  'Paid in cash'
# a use site resolves to the definition and reports where it lives
expect "$fixture:43:18" 'fn add(a: int, b: int) -> int'
expect "$fixture:43:18" 'doc_hover.b:9'
# span-based precision: a parameter and a local resolve to their declared types
expect "$fixture:10:12" 'a: int'
expect "$fixture:10:12" 'parameter'
expect "$fixture:42:9"  'let p: Point'
expect "$fixture:42:9"  'local'
# a type annotation resolves as a type, not a same-named value
expect "$fixture:42:12" 'class Point'
# member access resolves through the receiver's declared type: self.x -> Point.x
expect "$fixture:29:21" 'Point.x: int'
expect "$fixture:29:21" 'The horizontal coordinate.'
# builtin method: signature comes from the registry, not a hardcoded list
expect "examples/tour.b:87:21" 'fn string.to_int() -> Result<int>'
# stdlib symbol: signature comes from the real beans source in lib/std
gcd=$(grep -n 'fn gcd' lib/std/math/math.b | head -1 | cut -d: -f1)
expect "lib/std/math/math.b:$gcd:8" 'pub fn gcd(a: int, b: int) -> int'
# whitespace / no identifier under the cursor -> no symbol
expect_none "$fixture:42:1"

echo "ok lsp-probe hover: signatures + /// docs across decls, uses, builtins, stdlib"
