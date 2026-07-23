import std.io
import std.result

struct Pair {
    left: i32
    right: i32
}

fn pass(value: Result<Pair>) -> Result<Pair> {
    return value
}

fn wrap<T>(value: T) -> Result<T> {
    return ok(value)
}

fn shifted(value: Result<Pair>) -> Result<Pair> {
    let pair: Pair = value?
    return ok(Pair { left: pair.left + 1, right: pair.right + 1 })
}

fn main() {
    let pair: Pair = Pair { left: 10, right: 20 }
    let good: Result<Pair> = ok(pair)
    let bad: Result<Pair> = err("no pair")

    io.println("inline result {good.is_ok()} {bad.is_ok()} {good == pass(good)}")
    io.println("result values {good.expect("missing").right} {shifted(good).expect("missing").left} {wrap(pair).expect("missing").right}")
    let mapped: Result<Pair> = result.map(good, fn(value: Pair) -> Pair {
        return Pair { left: value.left + 5, right: value.right + 5 }
    })
    io.println("result stdlib {mapped.expect("missing").right}")

    match shifted(bad) {
        ok(value) => { io.println("bad {value.left}") }
        err(error) => { io.println("result error {error.msg}") }
    }

    let nested: Option<Result<Pair>> = some(good)
    io.println("result nested {nested.expect("outer").expect("inner").left}")

    let captured: Result<Pair> = bad
    let reader: fn() -> Result<Pair> = fn() -> Result<Pair> { return captured }
    match reader() {
        ok(value) => { io.println("bad {value.right}") }
        err(error) => { io.println("result capture {error.msg}") }
    }

    var changing: Result<Pair> = err("old")
    changing = ok(Pair { left: 30, right: 40 })
    io.println("result assign {changing.expect("missing").right}")
}
