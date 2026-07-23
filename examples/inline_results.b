import std.io

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

    let scalar_source: Result<int> = ok(5)
    let scalar: Result<int> = scalar_source.map(
        fn(value: int) -> int { return value + 1 })
    let text_source: Result<string> = ok("bean")
    let text: Result<string> = text_source.and_then(
        fn(value: string) -> Result<string> { return ok("{value}s") })
    let recovered: Pair = bad.recover(
        fn(error: Error) -> Pair {
            return Pair { left: error.msg.len() as i32, right: 9 }
        })

    io.println("inline result {good.is_ok()} {bad.is_ok()} {good == pass(good)}")
    io.println("result values {good.expect("missing").right} {shifted(good).expect("missing").left} {wrap(pair).expect("missing").right}")
    let mapped: Result<Pair> = good.map(fn(value: Pair) -> Pair {
        return Pair { left: value.left + 5, right: value.right + 5 }
    })
    io.println("result stdlib {mapped.expect("missing").right}")
    io.println("result methods {scalar.expect("scalar")} {text.expect("text")} {recovered.left} {bad.map(fn(value: Pair) -> string { return "{value.left}" }).is_ok()}")

    let array_source: Result<[i32; 4]> = ok([5, 6, 7, 8])
    let mapped_array: Result<[i32; 4]> = array_source.map(
        fn(value: [i32; 4]) -> [i32; 4] { return value })
    io.println("result array {mapped_array.expect("array")[2]}")

    match shifted(bad) {
        ok(value) => { io.println("bad {value.left}") }
        err(error) => { io.println("result error {error.msg}") }
    }

    let nested: Option<Result<Pair>> = some(good)
    io.println("result nested {nested.expect("outer").expect("inner").left}")

    let nested_source: Result<Result<Pair>> = ok(good)
    let mapped_nested: Result<Result<Pair>> = nested_source.map(
        fn(value: Result<Pair>) -> Result<Pair> { return value })
    io.println("result mapped nested {mapped_nested.expect("outer").expect("inner").right}")

    let captured: Result<Pair> = bad
    let reader: fn() -> Result<Pair> = fn() -> Result<Pair> { return captured }
    match reader() {
        ok(value) => { io.println("bad {value.right}") }
        err(error) => { io.println("result capture {error.msg}") }
    }

    var changing: Result<Pair> = err("old")
    changing = ok(Pair { left: 30, right: 40 })
    io.println("result assign {changing.expect("missing").right}")

    let wide_error: Result<Pair, [i32; 4]> = err([7, 8, 9, 10])
    let wide_error_map: Result<string, [i32; 4]> = wide_error.map(
        fn(value: Pair) -> string { return "{value.left}" })
    let wide_error_chain: Result<string, [i32; 4]> = wide_error.and_then(
        fn(value: Pair) -> Result<string, [i32; 4]> {
            return ok("{value.right}")
        })
    let wide_error_recover: Pair = wide_error.recover(
        fn(error: [i32; 4]) -> Pair {
            return Pair { left: error[0], right: error[3] }
        })
    match wide_error_map {
        ok(value) => { io.println("bad {value}") }
        err(error) => {
            io.println("result wide error {error[2]} {wide_error_chain.is_ok()} {wide_error_recover.left} {wide_error_recover.right}")
        }
    }
    let string_error: Result<int, string> = err("plain")
    let string_recovered: int = string_error.recover(
        fn(error: string) -> int { return error.len() })
    io.println("result custom error {string_error.is_ok()} {string_recovered}")

    unsafe {
        let vector_source: Result<Simd4f32> = ok(Simd4f32.of(1.0, 2.0, 3.0, 4.0))
        let vector: Result<Simd4f32> = vector_source.map(
            fn(value: Simd4f32) -> Simd4f32 { return value })
        let memory: RawPtr<i32> = RawPtr.alloc(2)
        memory.write(31)
        memory.offset(1).write(32)
        let view_source: Result<Slice<i32>> = ok(Slice.from_raw(memory, 2))
        let view: Result<Slice<i32>> = view_source.map(
            fn(value: Slice<i32>) -> Slice<i32> { return value })
        io.println("result wide {vector.expect("vector").sum()} {view.expect("view")[1]}")
        memory.free()
    }
}
