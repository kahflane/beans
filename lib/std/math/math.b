// High-level numeric helpers written in Beans, not C++.

pub fn clamp_int(value: int, low: int, high: int) -> int {
    if value < low {
        return low
    }
    if value > high {
        return high
    }
    return value
}

pub fn gcd(a: int, b: int) -> int {
    var x: int = a.abs()
    var y: int = b.abs()
    for y != 0 {
        let next: int = x % y
        x = y
        y = next
    }
    return x
}
