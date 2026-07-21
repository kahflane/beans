// stdlib phase 0 validation: `?` in the middle of a chain, map index reads,
// statics chaining into methods, and string method chains — run vs build
// must be byte-identical, panics included.
import std.io

class Wallet {
    pub cents: int = 0

    pub fn new(c: int) -> Wallet {
        return Wallet { cents: c }
    }

    pub fn add(self, c: int) -> Wallet {
        return Wallet.new(self.cents + c)
    }

    pub fn show(self) -> string {
        return "{self.cents}c"
    }
}

fn parse_both(a: string, b: string) -> Result<int> {
    // `?` mid-chain: unwrap the Result, keep chaining on the int
    let x: int = a.to_int()?.abs()
    let y: int = b.to_int()?.abs()
    return ok(x + y)
}

fn main() {
    // statics -> methods -> methods
    io.println(Wallet.new(120).add(80).add(50).show())

    // string chains
    let s: string = "beans language"
    io.println("{s.last(8).len()}")
    let has: bool = s.last(8).contains("gua")
    io.println("{has}")

    // map index reads, chained onward
    var m: Map<string, Wallet> = {}
    m["a"] = Wallet.new(500)
    m["b"] = m["a"].add(25)
    io.println(m["b"].show())
    let total: int = m["a"].cents + m["b"].cents
    io.println("{total}")

    var byid: Map<int, string> = {13: "riak", 42: "beans"}
    io.println(byid[42].last(3))

    // `?` chains, both arms
    match parse_both("-7", "35") {
        ok(v) => io.println("sum {v}"),
        err(e) => io.println("no: {e.msg}"),
    }
    match parse_both("12", "x9") {
        ok(v) => io.println("sum {v}"),
        err(e) => io.println("no: {e.msg}"),
    }

    // a doubled-up string keeps its length straight (O(1) len path)
    var big: string = "x"
    for i: int in 0..14 {
        big = "{big}{big}"
    }
    io.println("{big.len()}")

    // a missing key panics with the key in the message
    io.println(m["ghost"].show())
}
