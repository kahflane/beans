// returns.b — every shape the "missing return" check must keep accepting.
// beans has no implicit tail return, so a `-> T` body has to return on every
// path; the checker proves it before either backend runs. This file is the
// guard against that proof getting too strict — if a function here starts
// failing to check, the analysis grew a false positive.
import std.io

fn flag() -> bool { return true }

// both sides of an if/else return
fn branches() -> int {
    if flag() { return 1 } else { return 2 }
}

// else-if chain, every leg returns
fn chain(n: int) -> string {
    if n < 0 { return "neg" } else if n == 0 { return "zero" } else { return "pos" }
}

// statement match, every arm a returning block
fn arms() -> int {
    match flag() {
        true => { return 10 }
        false => { return 20 }
    }
}

// an arm holding an if/else that returns on both sides
fn nested() -> int {
    match flag() {
        true => { if flag() { return 30 } else { return 31 } }
        false => { return 32 }
    }
}

// `for { }` with no break never finishes, so nothing follows it
fn spins() -> int {
    var i: int = 0
    for {
        i = i + 1
        if i > 3 { return i }
    }
}

// the inner break belongs to the inner loop — the outer one still never exits
fn inner_break() -> int {
    var i: int = 0
    for {
        for {
            i = i + 1
            if i > 2 { break }
        }
        return i
    }
}

// a closure carries its own return type and its own obligation
fn via_closure() -> int {
    let dbl: fn(int) -> int = fn(x: int) -> int { return x * 2 }
    return dbl(21)
}

fn main() {
    io.println(branches())
    io.println(chain(0 - 5))
    io.println(chain(0))
    io.println(chain(7))
    io.println(arms())
    io.println(nested())
    io.println(spins())
    io.println(inner_break())
    io.println(via_closure())
}
