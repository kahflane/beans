import std.io

fn main() {
    let fixed: int = 7
    let read_fixed: fn() -> int = fn() -> int { return fixed }

    var changed: int = 1
    let read_changed: fn() -> int = fn() -> int { return changed }
    changed = 9

    var mutated: int = 20
    let bump: fn() -> int = fn() -> int {
        mutated += 1
        return mutated
    }

    io.println("{read_fixed()} {read_changed()} {bump()} {bump()}")
}
