// High-level numeric helpers written in Beans, not C++.

/// Clamp `value` into the inclusive range `[low, high]`.
///
/// When to use: keeping an index or measurement within known bounds without
/// writing the two comparisons by hand. Returns `low` if `value < low`, `high`
/// if `value > high`, otherwise `value` unchanged.
pub fn clamp_int(value: int, low: int, high: int) -> int {
    if value < low {
        return low
    }
    if value > high {
        return high
    }
    return value
}

/// Greatest common divisor of `a` and `b`, using the Euclidean algorithm.
///
/// When to use: reducing fractions or finding a common step size. Operates on
/// absolute values, so signs of the inputs do not affect the result; `gcd(0, 0)`
/// is `0`.
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
