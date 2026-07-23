import std.io
import std.os

interface Shape {
    fn area() -> f64
}

class Circle implements Shape {
    r: f64
    fn init(r: f64) { self.r = r }
    fn area() -> f64 { return 3.14159265 * self.r * self.r }
}

class Square implements Shape {
    s: f64
    fn init(s: f64) { self.s = s }
    fn area() -> f64 { return self.s * self.s }
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(100_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    let tweak: f64 = ((seed % 7) as f64) / 100.0
    let shape_count: int = 8 + seed % 8
    var shapes: List<Shape> = []
    shapes.reserve(shape_count)
    var made: int = 0
    for made < shape_count {
        if made % 2 == 0 {
            shapes.push(new Circle(1.5 + tweak))
        } else {
            shapes.push(new Square(2.0 + tweak))
        }
        made += 1
    }
    let rounds: int = n / shape_count
    var total: f64 = 0.0
    var i: int = 0
    for i < rounds {
        for s: Shape in shapes {
            total += s.area()
        }
        i += 1
    }
    io.println(total.round())
}
