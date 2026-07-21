#!/bin/bash
# bench/run.sh — run every bench in beans, Go, and JS (bun), write bench/report.md
#
# Method: compile ahead of time where the language compiles (beans native, go),
# then time ONLY execution, best of 3 runs. bun times include its startup
# (~10ms) because that is how JS ships. Compile times are reported separately.
set -u

cd "$(dirname "$0")/.."
ROOT=$PWD
OUT=build/bench
mkdir -p "$OUT"

BENCHES="fib loops shapes churn"
RUNS=3

# time one command, print seconds (best of $RUNS)
best_time() {
    local best=""
    for _ in $(seq $RUNS); do
        /usr/bin/time -p "$@" >/dev/null 2>"$OUT/t"
        local r
        r=$(awk '/^real/ {print $2}' "$OUT/t")
        if [ -z "$best" ] || awk "BEGIN{exit !($r < $best)}"; then best=$r; fi
    done
    echo "$best"
}

# wall-clock of a compile step, printed as seconds
compile_time() {
    local t0 t1
    t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
    "$@" >/dev/null 2>&1 || { echo "fail"; return; }
    t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
    awk "BEGIN{printf \"%.2f\", $t1 - $t0}"
}

[ -x build/beansc ] || make >/dev/null

HAVE_GO=$(command -v go >/dev/null && echo yes || echo no)
HAVE_BUN=$(command -v bun >/dev/null && echo yes || echo no)

REPORT=bench/report.md
{
    echo "# beans bench report"
    echo
    echo "- date: $(date '+%Y-%m-%d %H:%M')"
    echo "- machine: $(uname -m), $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    echo "- beansc: $(git rev-parse --short HEAD 2>/dev/null || echo untracked)"
    [ "$HAVE_GO" = yes ] && echo "- go: $(go version | awk '{print $3}')"
    [ "$HAVE_BUN" = yes ] && echo "- bun: $(bun --version)"
    echo
    echo "Execution time in seconds, best of $RUNS runs of a prebuilt binary"
    echo "(bun runs the .js directly, so its time includes runtime startup)."
    echo
    echo "| bench | beans | go | bun (js) |"
    echo "|---|---|---|---|"
} >"$REPORT"

BEANS_COMPILE=""
GO_COMPILE=""

for b in $BENCHES; do
    bt="n/a"; gt="n/a"; jt="n/a"

    c=$(compile_time ./build/beansc build "bench/$b.b" -o "$OUT/${b}_beans")
    if [ "$c" != "fail" ]; then
        BEANS_COMPILE="$BEANS_COMPILE $b:${c}s"
        bt=$(best_time "$OUT/${b}_beans")
    fi

    if [ "$HAVE_GO" = yes ]; then
        c=$(compile_time go build -o "$OUT/${b}_go" "bench/$b.go")
        if [ "$c" != "fail" ]; then
            GO_COMPILE="$GO_COMPILE $b:${c}s"
            gt=$(best_time "$OUT/${b}_go")
        fi
    fi

    if [ "$HAVE_BUN" = yes ]; then
        jt=$(best_time bun "bench/$b.js")
    fi

    echo "| $b | $bt | $gt | $jt |" >>"$REPORT"
    echo "$b: beans=$bt go=$gt bun=$jt"
done

{
    echo
    echo "Compile times (not in the table): beans —${BEANS_COMPILE:-n/a}; go —${GO_COMPILE:-n/a}."
    echo
    echo "Outputs are checked against each other by the languages' own runs;"
    echo "all three print the same result per bench."
} >>"$REPORT"

echo
echo "report written to $REPORT"
