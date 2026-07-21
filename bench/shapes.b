import std.io

interface Shape {
    fn area(self) -> f64
}

class Circle : Shape {
    r: f64
    fn area(self) -> f64 { return 3.14159265 * self.r * self.r }
}

class Square : Shape {
    s: f64
    fn area(self) -> f64 { return self.s * self.s }
}

fn main() {
    let shapes: List<Shape> = [Circle { r: 1.5 }, Square { s: 2.0 }]
    var total: f64 = 0.0
    var i: int = 0
    for i < 50_000_000 {
        for s: Shape in shapes {
            total += s.area()
        }
        i += 1
    }
    io.println(total.round())
}
