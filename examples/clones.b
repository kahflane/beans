import std.io

fn main() {
    var original: List<string> = ["one", "two"]
    var copied: List<string> = original.clone()
    copied.push("three")
    copied.remove(0)
    io.println("{original} | {copied}")

    var counts: Map<string, int> = {"one": 1, "two": 2}
    var counts_copy: Map<string, int> = counts.clone()
    counts_copy["one"] = 10
    counts_copy["three"] = 3
    counts_copy.remove("two")
    io.println("{counts.len()} {counts.get("one").or(-1)} {counts.contains("three")}")
    io.println("{counts_copy.len()} {counts_copy.get("one").or(-1)} {counts_copy.contains("two")}")

    var ordered: OrderedMap<int, string> = {1: "first", 2: "second"}
    var ordered_copy: OrderedMap<int, string> = ordered.clone()
    ordered_copy.remove(1)
    ordered_copy[1] = "again"
    io.println("{ordered.keys()} | {ordered_copy.keys()}")
}
