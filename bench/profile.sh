#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
name=${1:-}
if [[ -z "$name" ]] || ! awk -F '\t' -v n="$name" '$1 == n { found=1 } END { exit !found }' bench/suite.tsv; then
    echo "unknown benchmark: ${name:-<empty>}" >&2
    exit 2
fi

out=build/bench/profile
mkdir -p "$out"
size=$(awk -F '\t' -v n="$name" '$1 == n { print $4 }' bench/suite.tsv)
seed=$(awk -F '\t' -v n="$name" '$1 == n { print $5 }' bench/suite.tsv)
stamp=$(date +%Y%m%d-%H%M%S)

./build/beansc build --release --lto --cpu native "bench/$name.b" \
    -o "$out/${name}_shipping"
clang -O3 -march=native -pthread -DBEANS_ARC_STATS -Wno-override-module \
    "build/$name.ll" build/beans_rt.c -o "$out/${name}_arc"

"$out/${name}_arc" "$size" "$seed" >"$out/${name}-arc.stdout" \
    2>"$out/${name}-arc.txt"

profile_base="$out/${name}-${stamp}"
case "$(uname -s)" in
    Darwin)
        if xcrun --find xctrace >/dev/null 2>&1 && \
            xcrun xctrace record --template 'Time Profiler' --time-limit 5s \
                --output "$profile_base.trace" --launch -- \
                "$out/${name}_shipping" "$size" "$seed"; then
            echo "profile: $profile_base.trace"
        else
            /usr/bin/time -l "$out/${name}_shipping" "$size" "$seed" \
                >"$profile_base.stdout" 2>"$profile_base.time.txt"
            echo "profile fallback: $profile_base.time.txt"
        fi
        ;;
    Linux)
        if command -v perf >/dev/null 2>&1 && \
            perf record -g -o "$profile_base.data" -- \
                "$out/${name}_shipping" "$size" "$seed"; then
            perf report --stdio -i "$profile_base.data" >"$profile_base.txt"
            echo "profile: $profile_base.data and $profile_base.txt"
        else
            /usr/bin/time -v "$out/${name}_shipping" "$size" "$seed" \
                >"$profile_base.stdout" 2>"$profile_base.time.txt"
            echo "profile fallback: $profile_base.time.txt"
        fi
        ;;
    *)
        echo "no profiler recipe for $(uname -s)" >&2
        exit 1
        ;;
esac

echo "ARC statistics: $out/${name}-arc.txt"
cat "$out/${name}-arc.txt"
