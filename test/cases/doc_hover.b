// Fixture for `lsp-probe`: /// doc comments on every kind of declaration.

import std.io

/// Adds two integers and returns the sum.
///
/// When to use: the simplest documented function — hover should render this
/// whole block as Markdown.
fn add(a: int, b: int) -> int {
    return a + b
}

/// A point in 2-D space.
///
/// When to use: hover the type name for this; hover a field or method for
/// those docs instead.
class Point {
    /// The horizontal coordinate.
    x: int
    y: int

    fn init(x: int, y: int) {
        self.x = x
        self.y = y
    }

    /// Distance from the origin, squared.
    fn norm2() -> int {
        return self.x * self.x + self.y * self.y
    }
}

/// How a customer paid.
enum Payment {
    /// Paid in cash — no extra data.
    cash
    /// Paid by card; carries the card number.
    card(number: string)
}

fn main() {
    let p: Point = new Point(3, 4)
    let s: int = add(1, 2)
    io.println("{s} {p.norm2()}")
}
