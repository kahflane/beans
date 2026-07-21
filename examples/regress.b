// regress.b — pins the bugs fixed in the stdlib-audit pass. Every line here
// diverged between `beansc run` and the native binary, or crashed, before the
// fix. Kept in examples/ so the run-vs-native diff catches any regression.
import std.io
import std.fmt

// C4: structural equality for enums, Bytes, decimals, floats — native used to
// compare these by pointer identity (contains/index_of/map-key/==)
enum Box { of(v: int), empty }

fn eq_checks() {
    let boxes: List<Box> = [Box.of(1), Box.of(2), Box.empty]
    io.println("{boxes.contains(Box.of(2))} {boxes.contains(Box.empty)} {boxes.contains(Box.of(9))}")
    io.println("{boxes.index_of(Box.of(1)).or(0 - 1)}")
    io.println("{Box.of(1) == Box.of(1)} {Box.of(1) == Box.of(2)}")

    let bs: List<Bytes> = [Bytes.from("ab"), Bytes.from("cd")]
    io.println("{bs.contains(Bytes.from("ab"))} {bs.contains(Bytes.from("zz"))}")

    // decimal and float map keys (native panicked "key not found" on every read)
    var dm: Map<decimal, string> = {}
    dm[2.50] = "two-fifty"
    io.println(dm[2.5])
    var fm: Map<float, string> = {}
    fm[0.0] = "zero"
    io.println("{fm.contains(0.0 - 0.0)}")
}

// C1: decimal literals passed to builtins used to arrive as floats in interp
fn fmt_checks() {
    io.println(fmt.dec(19.995, 2))
    var ds: List<decimal> = [1.50]
    ds.push(2.250)
    io.println("{ds} {ds.contains(1.50)}")
}

// C5 / M8: high-scale decimals used to smash the native stack; round(negative)
// used to render bare digits instead of the magnitude
fn decimal_checks() {
    let tiny: decimal = "1e-40".to_decimal().expect("tiny")
    io.println("{tiny.round(40)}")
    let big: decimal = 1234.5
    let half: decimal = 1250.0
    io.println("{big.round(0 - 2)} {half.round(0 - 2)}")
    // M1: an absurd exponent is rejected identically on both backends
    match "1e100000".to_decimal() {
        ok(v) => io.println("parsed {v}"),
        err(e) => io.println("rejected: {e.msg}"),
    }
}

// H1: strings carry embedded NUL bytes; native print/compare/contains/last
// used to stop at the first NUL
fn nul_checks() {
    let s: string = "a\0b"
    io.println("{s.len()} {s == "a"} {s.contains("b")} {s.last(1)}")
}

// C6: a defer runs on frame exit, after the body, newest first — and used to
// run on already-freed locals
fn defer_checks() {
    let tag: string = "kept"
    defer io.println("defer-2 {tag}")
    defer io.println("defer-1 {tag}")
    io.println("body {tag}")
}

// C7: integer and decimal divide-by-zero panic with the same message on both
// backends (native used to return garbage). Guarded so main still completes.
fn div_checks() {
    var z: int = 0
    if z > 0 {
        io.println("{10 / z}")
    }
    io.println("div guarded")
}

fn main() {
    eq_checks()
    fmt_checks()
    decimal_checks()
    nul_checks()
    defer_checks()
    div_checks()
    io.println("regress ok")
}
