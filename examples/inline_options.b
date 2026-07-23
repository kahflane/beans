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

    io.println("inline option {present.is_some()} {empty.is_none()} {present == pass(present)} {empty == other_empty}")
    io.println("option values {present.expect("missing").right} {empty.or(fallback).left} {shifted(present).expect("missing").left} {shifted(empty).is_none()} {wrap(pair).expect("missing").right}")

    match present {
        some(value) => { io.println("option match {value.left} {value.right}") }
        none => { io.println("bad") }
    }

    let array: Option<[i32; 4]> = some([5, 6, 7, 8])
    io.println("option array {array.expect("missing")[2]}")

    let nested: Option<Option<Pair>> = some(some(pair))
    io.println("option nested {nested.expect("outer").expect("inner").right}")

    let captured: Option<Pair> = present
    let reader: fn() -> Option<Pair> = fn() -> Option<Pair> { return captured }
    io.println("option capture {reader().expect("missing").left}")

    unsafe {
        let vector: Option<Simd4f32> = some(Simd4f32.of(1.0, 2.0, 3.0, 4.0))
        io.println("option simd {vector.expect("missing").sum()}")

        let memory: RawPtr<i32> = RawPtr.alloc(2)
        memory.write(31)
        memory.offset(1).write(32)
        let view: Option<Slice<i32>> = some(Slice.from_raw(memory, 2))
        io.println("option slice {view.expect("missing")[1]}")
        memory.free()
    }
}
