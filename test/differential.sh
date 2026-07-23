#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/beans-diff.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

failed=0
for file in examples/*.b examples/shop/main.b; do
    name=$(basename "$file" .b)
    bin="$tmp/${name}_native"
    if ! ./build/beansc build "$file" -o "$bin" >"$tmp/${name}.build" 2>&1; then
        echo "build failed: $file" >&2
        sed -n '1,80p' "$tmp/${name}.build" >&2
        failed=1
        continue
    fi

    set +e
    ./build/beansc run "$file" >"$tmp/${name}.interp" 2>&1
    interp_status=$?
    "$bin" >"$tmp/${name}.native" 2>&1
    native_status=$?
    set -e

    if [[ "$interp_status" -ne "$native_status" ]] || \
        ! cmp -s "$tmp/${name}.interp" "$tmp/${name}.native"; then
        echo "differential failure: $file (run=$interp_status native=$native_status)" >&2
        diff -u "$tmp/${name}.interp" "$tmp/${name}.native" | sed -n '1,120p' >&2 || true
        failed=1
    else
        echo "ok $file"
    fi
done

exit "$failed"
