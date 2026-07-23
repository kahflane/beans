// Floating-point matrix/vector work with runtime dimensions.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(1600)
    let seed: int = args.get(1).or("").to_int().or(1)
    var matrix: List<f64> = []
    matrix.reserve(n * n)
    var row: int = 0
    for row < n {
        var col: int = 0
        for col < n {
            matrix.push((((row * 17 + col * 31 + seed) % 1000) as f64) / 1000.0)
            col += 1
        }
        row += 1
    }
    var input: List<f64> = []
    var output: List<f64> = []
    input.reserve(n)
    output.reserve(n)
    var i: int = 0
    for i < n {
        input.push(((i + seed) % 97) as f64)
        output.push(0.0)
        i += 1
    }
    var round: int = 0
    for round < 30 {
        row = 0
        for row < n {
            var total: f64 = 0.0
            var col: int = 0
            for col < n {
                if round % 2 == 0 {
                    total += matrix[row * n + col] * input[col]
                } else {
                    total += matrix[row * n + col] * output[col]
                }
                col += 1
            }
            if round % 2 == 0 {
                output[row] = total / (n as f64)
            } else {
                input[row] = total / (n as f64)
            }
            row += 1
        }
        round += 1
    }
    var checksum: f64 = 0.0
    for value: f64 in input { checksum += value }
    io.println("matrix {(checksum * 1_000_000.0).round()}")
}
