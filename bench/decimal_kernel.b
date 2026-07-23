// Exact fixed-scale ledger arithmetic.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(100_000_000) } else { 100_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    // Fix the ledger scale once. Codegen keeps this proven scale through the
    // loop and can use the inline coefficient path for both updates.
    var balance: decimal = (seed as decimal) + 0.00
    var i: int = 0
    for i < n {
        if (i + seed) % 3 == 0 { balance += 1.25 } else { balance -= 0.50 }
        i += 1
    }
    io.println("decimal_kernel {balance}")
}
