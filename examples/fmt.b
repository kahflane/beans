// stdlib phase 3: std.fmt, and printing that now covers lists and enums —
// io.println and interpolation render them exactly like the interpreter's
// display(): [1, 2, 3], variant(payload), nesting included.
import std.io
import std.fmt

enum Shape {
    circle(r: float),
    rect(w: float, h: float),
    dot,
}

fn main() {
    // pad / float / dec / hex / bin / group
    io.println("[{fmt.pad_left("42", 8)}]")
    io.println("[{fmt.pad_right("ab", 5)}]")
    io.println("[{fmt.pad_left("too long", 3)}]")
    io.println(fmt.float(3.14159, 2))
    io.println(fmt.float(-0.125, 4))
    let d: decimal = 19.995
    io.println("{fmt.dec(d, 2)} {fmt.dec(d, 5)}")
    let w: decimal = 7
    io.println(fmt.dec(w, 3))
    io.println("{fmt.hex(255)} {fmt.hex(-1)} {fmt.hex(0)}")
    io.println("{fmt.bin(5)} {fmt.bin(0)}")
    io.println(fmt.group(1234567, ","))
    io.println("{fmt.group(-42, "_")} {fmt.group(123, " ")} {fmt.group(0, ",")}")

    // a right-aligned table, the way fmt is meant to be used
    let items: List<string> = ["nails", "hammers", "glue"]
    let counts: List<int> = [12040, 7, 331]
    var i: int = 0
    for i < items.len() {
        io.println("{fmt.pad_right(items[i], 10)}{fmt.pad_left(fmt.group(counts[i], ","), 8)}")
        i += 1
    }

    // lists and enums print directly now
    let xs: List<int> = [1, 2, 3]
    io.println(xs)
    io.println("inline: {xs}")
    io.println([[1, 2], [], [3]])

    let s1: Shape = Shape.circle(2.5)
    let s2: Shape = Shape.rect(3.0, 4.5)
    io.println("{s1} {s2} {Shape.dot}")
    let shapes: List<Shape> = [s1, s2, Shape.dot]
    io.println(shapes)
    io.println(shapes.join(" | "))

    let maybe: Option<int> = some(42)
    let nada: Option<int> = none
    io.println("{maybe} {nada}")
    io.println([some(1), none, some(3)])
    let ol: Option<List<int>> = some([7, 8])
    io.println(ol)

    let bools: List<bool> = [true, false]
    let ds: List<decimal> = [1.50, 0.25]
    let fs: List<float> = [1.5, 2.0]
    io.println("{bools} {ds} {fs}")
}
