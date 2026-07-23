// mandelbrot: f64 arithmetic in a tight loop, zero allocation. There is no
// sqrt in the stdlib and none is needed — the escape test squares instead.
// Pure IEEE doubles, so every language must land on the same count.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let size: int = args.get(0).or("").to_int().or(1800)
    let seed: int = args.get(1).or("").to_int().or(1)
    let w: int = size
    let h: int = size
    let max_iter: int = 80 + seed % 41
    var inside: int = 0

    var y: int = 0
    for y < h {
        let ci: f64 = (y as f64) * 2.0 / (h as f64) - 1.0
        var x: int = 0
        for x < w {
            let cr: f64 = (x as f64) * 3.0 / (w as f64) - 2.0
            var zr: f64 = 0.0
            var zi: f64 = 0.0
            var i: int = 0
            for i < max_iter {
                let t: f64 = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                if zr * zr + zi * zi > 4.0 {
                    break
                }
                i += 1
            }
            if i == max_iter {
                inside += 1
            }
            x += 1
        }
        y += 1
    }

    io.println("mandel {inside}")
}
