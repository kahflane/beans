// Stable object sort. Equal keys must keep input order.
import std.io
import std.os

class Record {
    key: int
    order: int
}

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(2_000_000) } else { 2_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var records: List<Record> = []
    records.reserve(n)
    var i: int = 0
    for i < n {
        records.push(Record { key: (i * 48271 + seed) % 4096, order: i })
        i += 1
    }
    records.sort_by_key(fn(record: Record) -> int { return record.key })
    var checksum: int = 0
    i = 0
    for i < n {
        checksum += records[i].key * 17 + records[i].order
        i += 1
    }
    io.println("sort_objects {checksum} {records[0].key} {records[n - 1].key}")
}
