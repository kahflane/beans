// Inline four-lane f32 arithmetic. Runtime arguments keep the loop and seed
// opaque to LLVM; the checksum observes every lane.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(200_000_000) } else { 200_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    unsafe {
        var value: Simd4f32 = Simd4f32.of(seed as f32, (seed + 1) as f32,
                                           (seed + 2) as f32, (seed + 3) as f32)
        let scale: Simd4f32 = Simd4f32.of(0.99991, 0.99989, 0.99987, 0.99983)
        let delta: Simd4f32 = Simd4f32.of(0.011, 0.013, 0.017, 0.019)
        var index: int = 0
        for index < n {
            value = value * scale + delta
            index += 1
        }
        io.println("simd_kernel {value.lane(0).round()} {value.lane(1).round()} {value.lane(2).round()} {value.lane(3).round()} {value.sum().round()}")
    }
}
