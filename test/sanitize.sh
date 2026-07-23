#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
out=build/test
mkdir -p "$out"

run_asan() {
    local file=$1 name=$2 expected=${3:-0}
    echo "ASan checking $file"
    ./build/beansc build "$file" -o "$out/${name}_source" >/dev/null
    clang -O1 -g -pthread -fsanitize=address -Wno-override-module \
        "build/$name.ll" build/beans_rt.c -o "$out/${name}_asan"
    set +e
    BEANS_NO_POOL=1 "$out/${name}_asan" >"$out/${name}.stdout" \
        2>"$out/${name}.stderr"
    local status=$?
    set -e
    if [[ "$status" -ne "$expected" ]] || grep -q 'AddressSanitizer' "$out/${name}.stderr"; then
        echo "ASan failed: $file (status $status, expected $expected)" >&2
        sed -n '1,160p' "$out/${name}.stderr" >&2
        return 1
    fi
    echo "ASan ok $file"
}

run_asan bench/trees.b trees
run_asan examples/cycles.b cycles
run_asan examples/deep.b deep
run_asan examples/box.b box
run_asan examples/arena.b arena
run_asan examples/containers.b containers 3
run_asan test/cases/map_models.b map_models
run_asan examples/shared_weak.b shared_weak
run_asan examples/unsafe_raw.b unsafe_raw
run_asan examples/simd.b simd
run_asan examples/fixed_arrays.b fixed_arrays
run_asan examples/raw_slices.b raw_slices
run_asan examples/c_layout_structs.b c_layout_structs
run_asan examples/c_layout_unions.b c_layout_unions
run_asan examples/inline_options.b inline_options
run_asan examples/inline_results.b inline_results
run_asan examples/wide_lists.b wide_lists
run_asan examples/wide_maps.b wide_maps
run_asan examples/wide_enums.b wide_enums
run_asan examples/wide_owners.b wide_owners
run_asan examples/wide_sync.b wide_sync
run_asan examples/wide_concurrency.b wide_concurrency
run_asan examples/stdlib_beans.b stdlib_beans
run_asan examples/ffi.b ffi
run_asan test/cases/move_ok.b move_ok
run_asan examples/regress_mem.b regress_mem 3

for file in examples/threads.b examples/shared_weak.b examples/wide_sync.b \
            examples/wide_concurrency.b examples/unsafe_raw.b; do
    echo "TSan checking $file"
    name=$(basename "$file" .b)
    ./build/beansc build "$file" -o "$out/${name}_source" >/dev/null
    if clang -O1 -g -pthread -fsanitize=thread -Wno-override-module \
        "build/$name.ll" build/beans_rt.c -o "$out/${name}_tsan"; then
        BEANS_NO_POOL=1 "$out/${name}_tsan" >"$out/${name}.stdout" \
            2>"$out/${name}.stderr"
        if grep -q 'WARNING: ThreadSanitizer' "$out/${name}.stderr"; then
            sed -n '1,200p' "$out/${name}.stderr" >&2
            exit 1
        fi
        echo "TSan ok $file"
    else
        echo "TSan unavailable for $file; skipped" >&2
    fi
done

if [[ "$(uname -s)" == Darwin ]] && command -v leaks >/dev/null 2>&1; then
    for file in bench/trees.b examples/box.b examples/arena.b examples/fmt.b \
                examples/shared_weak.b examples/inline_results.b examples/wide_lists.b \
                examples/wide_maps.b examples/wide_enums.b examples/wide_owners.b \
                examples/wide_sync.b examples/wide_concurrency.b \
                examples/stdlib_beans.b \
                test/cases/map_models.b; do
        echo "leaks checking $file"
        name=$(basename "$file" .b)
        ./build/beansc build "$file" -o "$out/${name}_leaks" >/dev/null
        BEANS_NO_POOL=1 leaks --atExit -- "$out/${name}_leaks" 14 17 \
            >"$out/${name}.leaks" 2>&1
        if ! grep -q '0 leaks for 0 total leaked bytes' "$out/${name}.leaks"; then
            tail -80 "$out/${name}.leaks" >&2
            exit 1
        fi
        echo "leaks ok $file"
    done
fi
