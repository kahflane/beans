#!/usr/bin/env bash
# Build every target first, then hand timing to the C++ runner.
set -euo pipefail

cd "$(dirname "$0")/.."
root=$PWD
mode=${1:-quick}
case "$mode" in
    quick|full|verify) ;;
    *) echo "usage: bench/run.sh <quick|full|verify>" >&2; exit 2 ;;
esac

out=build/bench
mkdir -p "$out"
manifest=bench/suite.tsv
compile_times="$out/compile-times.tsv"
cxx=${CXX:-clang++}
cxx_flags=(-std=c++20 -O3 -DNDEBUG -flto -march=native -ffp-contract=off -pthread)
beans_flags=(--release --lto --cpu native)
runner_flags=(-std=c++20 -O2 -Wall -Wextra)

elapsed() {
    local start end
    start=$(perl -MTime::HiRes=time -e 'printf "%.9f", time')
    if ! "${@:2}" >&2; then
        return 1
    fi
    end=$(perl -MTime::HiRes=time -e 'printf "%.9f", time')
    perl -e 'printf "%.6f", $ARGV[1] - $ARGV[0]' "$start" "$end"
}

make -s build/beansc
"$cxx" "${runner_flags[@]}" bench/runner.cpp -o "$out/runner"
# Compile the cached runtime bitcode before per-workload compile timing.
./build/beansc build "${beans_flags[@]}" bench/fib.b -o "$out/runtime-prime" >/dev/null
printf '# workload\ttarget\tseconds\n' >"$compile_times"

mapfile_compat() {
    while IFS=$'\t' read -r name group quick size seed expected scored; do
        [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
        [[ "$mode" == "quick" && "$quick" != "1" ]] && continue
        printf '%s\n' "$name"
    done <"$manifest"
}

while IFS= read -r name; do
    printf 'building %s\n' "$name"
    # A failed build must stop before the runner can see an older binary with
    # the same name. `elapsed` returns the compiler status on purpose.
    seconds=$(elapsed beans ./build/beansc build "${beans_flags[@]}" "bench/$name.b" -o "$out/${name}_beans")
    printf '%s\tbeans\t%s\n' "$name" "$seconds" >>"$compile_times"

    seconds=$(elapsed cpp_tuned "$cxx" "${cxx_flags[@]}" "bench/cpp/$name.cpp" -o "$out/${name}_cpp_tuned")
    printf '%s\tcpp_tuned\t%s\n' "$name" "$seconds" >>"$compile_times"

    seconds=$(elapsed cpp_matched "$cxx" "${cxx_flags[@]}" -DBEANS_MATCHED=1 "bench/cpp/$name.cpp" -o "$out/${name}_cpp_matched")
    printf '%s\tcpp_matched\t%s\n' "$name" "$seconds" >>"$compile_times"

    if command -v go >/dev/null 2>&1 && [[ -f "bench/$name.go" ]]; then
        seconds=$(elapsed go go build -o "$out/${name}_go" "bench/$name.go")
        printf '%s\tgo\t%s\n' "$name" "$seconds" >>"$compile_times"
    fi
done < <(mapfile_compat)

export BENCH_CXX_FLAGS="${cxx_flags[*]}"
export BENCH_BEANS_FLAGS="beansc build ${beans_flags[*]}"
"$out/runner" "$mode" "$manifest" "$out" "$compile_times" \
    "$out/results-$mode.json" bench/report.md "$root"
