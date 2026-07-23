// Build and parse CSV-like log rows, then aggregate with a string-key map.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(1_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var rows: List<string> = []
    rows.reserve(n)
    var i: int = 0
    for i < n {
        let level: string = if (i + seed) % 7 == 0 { "error" } else { "info" }
        rows.push("user{(i * 17 + seed) % 4096},{level},{(i * 31 + seed) % 1000}")
        i += 1
    }
    var users: Map<string, int> = {}
    users.reserve(4096)
    var total: int = 0
    var errors: int = 0
    for row: string in rows {
        let first: int = row.find_byte(44, 0)
        let second: int = row.find_byte(44, first + 1)
        let user: string = row.slice(0, first)
        users[user] = users.get(user).or(0) + 1
        if row.range_equals(first + 1, second, "error") { errors += 1 }
        total += row.parse_int_range_or(second + 1, row.len(), 0)
    }
    io.println("log_aggregate {users.len()} {errors} {total}")
}
