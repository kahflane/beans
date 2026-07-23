// Formatting algorithms written in Beans. Floating-point and decimal
// conversion remain native primitives for now.

fn hex_impl(value: int) -> string {
    let digits: string = "0123456789abcdef"
    var remaining: u64 = value as u64
    if remaining == 0 { return "0" }
    var out: Bytes = Bytes.new(16)
    var index: int = 16
    for remaining != 0 {
        index -= 1
        let digit: int = (remaining & 15) as int
        out.set(index, digits.byte_at(digit))
        remaining = remaining >> 4
    }
    return out.slice(index, 16).to_string()
}

fn bin_impl(value: int) -> string {
    var remaining: u64 = value as u64
    if remaining == 0 { return "0" }
    var out: Bytes = Bytes.new(64)
    var index: int = 64
    for remaining != 0 {
        index -= 1
        out.set(index, 48 + ((remaining & 1) as int))
        remaining = remaining >> 1
    }
    return out.slice(index, 64).to_string()
}

fn magnitude_text(value: int) -> string {
    var magnitude: u64 = value as u64
    if value < 0 { magnitude = (0 as u64) - magnitude }
    if magnitude == 0 { return "0" }
    var out: Bytes = Bytes.new(20)
    var index: int = 20
    for magnitude != 0 {
        index -= 1
        out.set(index, 48 + ((magnitude % 10) as int))
        magnitude = magnitude / 10
    }
    return out.slice(index, 20).to_string()
}

fn group_impl(value: int, separator: string) -> string {
    let digits: string = magnitude_text(value)
    var parts: List<string> = []
    if value < 0 { parts.push("-") }
    var index: int = 0
    let leading: int = digits.len() % 3
    if leading != 0 {
        parts.push(digits.slice(0, leading))
        index = leading
    }
    for index < digits.len() {
        if index != 0 { parts.push(separator) }
        parts.push(digits.slice(index, index + 3))
        index += 3
    }
    return parts.join("")
}

pub fn hex(value: int) -> string { return hex_impl(value) }
pub fn bin(value: int) -> string { return bin_impl(value) }
pub fn group(value: int, separator: string) -> string {
    return group_impl(value, separator)
}
