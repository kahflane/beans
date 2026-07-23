import std.io

struct Pair {
    left: i32
    right: i32
}

fn pass(value: Option<Pair>) -> Option<Pair> {
    return value
}

fn wrap<T>(value: T) -> Option<T> {
    return some(value)
}

fn shifted(value: Option<Pair>) -> Option<Pair> {
    let pair: Pair = value?
    return some(Pair { left: pair.left + 1, right: pair.right + 1 })
}

fn main() {
    let pair: Pair = Pair { left: 10, right: 20 }
    let present: Option<Pair> = some(pair)
    let empty: Option<Pair> = none
    let other_empty: Option<Pair> = none
    let fallback: Pair = Pair { left: 3, right: 4 }

    let scalar: Option<int> = some(5).map(fn(value: int) -> int {
        return value + 1
    })
    let text: Option<string> = some("bean").and_then(
        fn(value: string) -> Option<string> { return some("{value}s") })
    let pair_map: Option<Pair> = present.map(fn(value: Pair) -> Pair {
        return Pair { left: value.left + 2, right: value.right + 3 }
    })
    let filtered: Option<Pair> = present.filter(
        fn(value: Pair) -> bool { return value.right == 20 })

    io.println("inline option {present.is_some()} {empty.is_none()} {present == pass(present)} {empty == other_empty}")
    io.println("option values {present.expect("missing").right} {empty.or(fallback).left} {shifted(present).expect("missing").left} {shifted(empty).is_none()} {wrap(pair).expect("missing").right}")
    io.println("option methods {scalar.or(0)} {text.or("bad")} {pair_map.expect("pair").right} {filtered.is_some()} {empty.map(fn(value: Pair) -> string { return "{value.left}" }).is_none()}")

    match present {
        some(value) => { io.println("option match {value.left} {value.right}") }
        none => { io.println("bad") }
    }

    let array: Option<[i32; 4]> = some([5, 6, 7, 8])
    let mapped_array: Option<[i32; 4]> = array.map(
        fn(value: [i32; 4]) -> [i32; 4] { return value })
    io.println("option array {array.expect("missing")[2]} {mapped_array.expect("mapped")[3]}")

    let nested: Option<Option<Pair>> = some(some(pair))
    let mapped_nested: Option<Option<Pair>> = nested.map(
        fn(value: Option<Pair>) -> Option<Pair> { return value })
    io.println("option nested {nested.expect("outer").expect("inner").right} {mapped_nested.expect("mapped outer").expect("mapped inner").left}")

    let captured: Option<Pair> = present
    let reader: fn() -> Option<Pair> = fn() -> Option<Pair> { return captured }
    io.println("option capture {reader().expect("missing").left}")

    unsafe {
        let vector: Option<Simd4f32> = some(Simd4f32.of(1.0, 2.0, 3.0, 4.0))
        let mapped_vector: Option<Simd4f32> = vector.map(
            fn(value: Simd4f32) -> Simd4f32 { return value })
        io.println("option simd {vector.expect("missing").sum()} {mapped_vector.expect("mapped").sum()}")

        let memory: RawPtr<i32> = RawPtr.alloc(2)
        memory.write(31)
        memory.offset(1).write(32)
        let view: Option<Slice<i32>> = some(Slice.from_raw(memory, 2))
        let mapped_view: Option<Slice<i32>> = view.map(
            fn(value: Slice<i32>) -> Slice<i32> { return value })
        io.println("option slice {view.expect("missing")[1]} {mapped_view.expect("mapped")[0]}")
        memory.free()
    }
}
