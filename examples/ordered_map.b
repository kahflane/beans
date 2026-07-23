// OrderedMap promises insertion order. Removing and reinserting moves a key
// to the end. Its storage ABI currently shares the mature Map runtime.
import std.io

fn main() {
    var ordered: OrderedMap<string, int> = {}
    ordered["b"] = 2
    ordered["a"] = 1
    ordered["c"] = 3
    ordered["b"] = 20
    ordered.remove("a")
    ordered["a"] = 10
    io.println(ordered.keys())
    io.println(ordered.values())
    io.println("{ordered.get("b").or(0)} {ordered.len()}")
}
