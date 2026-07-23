// Stable object sort. Equal keys must keep input order.
import std.io
import std.os

class Record {
    key: int
    order: int

    fn init(key: int, order: int) {
        self.key = key
        self.order = order
    }
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(2_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var records: List<Record> = []
    records.reserve(n)
    var i: int = 0
    for i < n {
        records.push(new Record((i * 48271 + seed) % 4096, i))
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
