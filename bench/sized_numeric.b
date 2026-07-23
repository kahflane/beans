// Exact narrow integers, unsigned operations, and f32 rounding.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(100_000_000) } else { 100_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var a: i8 = seed as i8
    var b: u16 = seed as u16
    var c: u32 = seed as u32
    var d: u64 = seed as u64
    var f: f32 = (seed as f32) / 10.0
    var i: int = 0
    for i < n {
        a = a + 17
        b = b * 3 + 1
        c = c * 1664525 + 1013904223
        d = d * 6364136223846793005 + 1
        f = f * 0.9999 + 0.0003
        i += 1
    }
    io.println("sized_numeric {a} {b} {c} {d} {f.round()}")
}
