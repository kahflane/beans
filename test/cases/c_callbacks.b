import std.io

extern "C" struct Point {
    x: i32
    y: i32
}

extern "C" fn beans_test_apply_twice(
    value: i32,
    callback: fn(i32, i32) -> i32
) -> i32
extern "C" fn beans_test_apply_two(
    value: i32,
    first: fn(i32) -> i32,
    second: fn(i32) -> i32
) -> i32
extern "C" fn beans_test_apply_f32(
    value: f32,
    callback: fn(f32, f32) -> f32
) -> f32
extern "C" fn beans_test_repeat(callback: fn(), count: u32) -> u32
extern "C" fn beans_test_map_point(
    value: Point,
    callback: fn(Point, i32) -> Point,
    amount: i32
) -> Point

fn digits(value: i32) -> i32 {
    return value * 10
}

fn plus_one(value: i32) -> i32 {
    return value + 1
}

fn main() {
    unsafe {
        let offset: i32 = 5
        let twice: i32 = beans_test_apply_twice(
            10,
            fn(value: i32, extra: i32) -> i32 {
                return value + extra + offset
            }
        )
        io.println("callback capture {twice}")

        let two: i32 = beans_test_apply_two(7, digits, plus_one)
        io.println("callback functions {two}")

        let scale: f32 = 2.0
        let floated: f32 = beans_test_apply_f32(
            1.5,
            fn(value: f32, extra: f32) -> f32 {
                return (value + extra) * scale
            }
        )
        io.println("callback float {floated}")

        var calls: i32 = 0
        let repeated: u32 = beans_test_repeat(fn() { calls += 2 }, 3)
        io.println("callback void {repeated} {calls}")

        let shifted: Point = beans_test_map_point(
            Point { x: 4, y: 9 },
            fn(value: Point, amount: i32) -> Point {
                return Point { x: value.x + amount, y: value.y - amount }
            },
            3
        )
        io.println("callback record {shifted.x} {shifted.y}")
    }
}
