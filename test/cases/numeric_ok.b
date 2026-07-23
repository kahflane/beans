import std.io

fn bump(v: i8) -> i8 {
    return v + 1
}

fn add_cent(v: decimal) -> decimal {
    return v + 0.01
}

class Tiny {
    value: i8
    fn init(value: i8) { self.value = value }
}

class Packed {
    a: u8
    b: i16
    c: u32
    d: u64
    f: f32

    fn init(a: u8, b: i16, c: u32, d: u64, f: f32) {
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.f = f
    }
}

class MoneyPad {
    tag: u8
    amount: decimal
    tail: u8

    fn init(tag: u8, amount: decimal, tail: u8) {
        self.tag = tag
        self.amount = amount
        self.tail = tail
    }
}

fn main() {
    var i: i8 = 127
    i += 1
    var u: u8 = 255
    u += 1
    var i16_max: i16 = 32767
    i16_max += 1
    var i32_max: i32 = 2147483647
    i32_max += 1
    var i64_max: i64 = 9223372036854775807
    i64_max += 1
    var u16_max: u16 = 65535
    u16_max += 1
    var u32_max: u32 = 4294967295
    u32_max += 1
    var u64_wrap: u64 = 18446744073709551615
    u64_wrap += 1
    let wide: u64 = 18446744073709551615
    let high: u64 = 9223372036854775808
    let narrow: u8 = 300 as u8
    let single: f32 = 16777217.0
    let half: f32 = 0.1
    let sum: f32 = half + 0.1
    let tiny: Tiny = new Tiny(127)
    let packed: Packed = new Packed(255, -32768, 4294967295,
        18446744073709551615, 0.1)
    let optional: Option<i8> = some(-1)
    let ordered: List<u64> = [wide, 1, high]
    ordered.sort()
    let floats: List<f32> = [3.5, 1.25, 2.0]
    floats.sort()
    var float_map: Map<f32, int> = {}
    float_map[0.5] = 7
    let money: MoneyPad = new MoneyPad(1, 19.99, 2)
    let decimals: List<decimal> = [1.25, 2.50]
    var decimal_map: Map<decimal, int> = {}
    decimal_map[2.5] = 9
    var ledger: decimal = (7 as decimal) + 0.00
    var step: int = 0
    for step < 10 {
        if (step + 7) % 3 == 0 { ledger += 1.25 } else { ledger -= 0.50 }
        step += 1
    }

    io.println("{i} {u} {bump(127)}")
    io.println("{i16_max} {i32_max} {i64_max}")
    io.println("{u16_max} {u32_max} {u64_wrap}")
    io.println("{wide} {high > 1} {high / 2}")
    io.println("{narrow} {single} {sum}")
    io.println("{tiny.value} {ordered}")
    io.println("{packed.a} {packed.b} {packed.c} {packed.d} {packed.f}")
    io.println("{optional} {floats} {float_map[0.5]}")
    io.println("{add_cent(money.amount)} {money.tail} {decimals} {decimal_map[2.50]}")
    io.println(ledger)
}
