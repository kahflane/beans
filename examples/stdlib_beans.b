import std.collections
import std.bytes as byte_algo
import std.io
import std.fmt
import std.math
import std.option
import std.path
import std.result

struct Pair {
    left: i32
    right: i32
}

fn main() {
    let numbers: List<int> = [1, 2, 2, 3]
    let words: List<string> = ["red", "blue", "red", "green", "blue"]
    let counts: Map<string, int> = collections.frequencies(words)

    io.println("{collections.sum_int(numbers)} {collections.count_int(numbers, 2)}")
    io.println("{counts["red"]} {counts["blue"]} {collections.unique(words)}")
    io.println("{math.clamp_int(12, 0, 10)} {math.gcd(84, 30)}")

    let evens: List<int> = collections.filter(numbers, fn(value: int) -> bool {
        return value % 2 == 0
    })
    let labels: List<string> = collections.transform(evens, fn(value: int) -> string {
        return "n{value}"
    })
    io.println("{collections.count(numbers, 2)} {collections.unique_of(numbers)} {labels}")

    var inventory: Map<string, int> = {"beans": 2, "rice": 4}
    let incremented: int = collections.increment(inout inventory, "beans", 3)
    let created: int = collections.get_or_insert(
        inout inventory, "tea", fn() -> int { return 7 })
    let existing: int = collections.get_or_insert(
        inout inventory, "tea", fn() -> int { return 99 })
    let incoming: Map<string, int> = {"beans": 10, "fruit": 6}
    collections.merge_with(inout inventory, incoming,
        fn(left: int, right: int) -> int { return left + right })
    let removed: int = collections.remove_if(inout inventory,
        fn(key: string, value: int) -> bool {
            return key == "rice" || value < 0
        })
    let inventory_text: Map<string, string> = collections.map_values(inventory,
        fn(key: string, value: int) -> string { return "{key}:{value}" })
    io.println("map source {incremented} {created} {existing} {inventory["beans"]} {inventory["tea"]} {removed} {inventory_text["fruit"]}")

    var wide_counts: Map<Pair, int> = {}
    let wide_total: int = collections.increment(
        inout wide_counts, Pair { left: -1, right: 2 }, 5)
    io.println("wide generic {wide_total} {wide_counts[Pair { left: -1, right: 2 }]}")

    let mapped: Option<string> = option.map(some(21), fn(value: int) -> string {
        return "value {value * 2}"
    })
    let kept: Option<int> = option.filter(some(8), fn(value: int) -> bool {
        return value > 5
    })
    io.println("{mapped.or("missing")} {kept.or(0)}")

    let parsed: Result<string> = result.map(ok(21), fn(value: int) -> string {
        return "result {value * 2}"
    })
    let missing: Result<int> = err("missing number")
    let recovered: int = result.recover(missing, fn(error: Error) -> int {
        return error.msg.len()
    })
    io.println("{parsed.expect("bad map")} {recovered}")

    let raw: Bytes = Bytes.from("123456789")
    let encoded: Bytes = byte_algo.encode_varint(300)
    let appended: Bytes = Bytes.new(1)
    appended.set(0, 99)
    byte_algo.append_varint(appended, 300)
    io.println("{byte_algo.crc32(raw)} {raw.crc32(0, raw.len()) as u32}")
    io.println("{byte_algo.varint_size(300)} {encoded.get(0)} {encoded.get(1)} {byte_algo.decode_varint(encoded).or(0)}")
    io.println("append {appended.len()} {byte_algo.decode_varint_at_or(appended, 1, 0)} {byte_algo.decode_varint_at_or(appended, 99, 77)}")

    let boundaries: List<u64> = [0, 1, 127, 128, 300, 16384, 18446744073709551615]
    var decoded_sum: u64 = 0
    var encoded_size: int = 0
    for value: u64 in boundaries {
        let item: Bytes = byte_algo.encode_varint(value)
        decoded_sum = decoded_sum + byte_algo.decode_varint(item).or(0)
        encoded_size += item.len()
    }
    let malformed: Bytes = Bytes.new(10).fill(255)
    io.println("boundaries {decoded_sum} {encoded_size} {byte_algo.decode_varint(malformed).or(77)}")
    io.println("path {path.join("a/", "b")} {path.join("a", "/root")} {path.parent("/a/b/")} {path.base("/a/b/")} {path.ext("archive.tar.gz")} {path.stem("archive.tar.gz")} {path.ext(".bashrc")}")
    io.println("fmt {fmt.hex(-1)} {fmt.bin(10)} {fmt.group(-9223372036854775807 - 1, "_")}")
}
