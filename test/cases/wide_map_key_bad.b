extern "C" union RawKey {
    signed: i64
    unsigned: u64
}

fn main() {
    let bad: Map<RawKey, int> = {}
    bad.len()
}
