// stdlib phase 3: MMap — a shared whole-file mapping with bounds-checked
// word access. put/get/read/write panic out of range, flush/close report
// Results, and dropping a map without close() unmaps it (kind-6 resource).
import std.io
import std.fs

fn main() {
    let p: string = "{Dir.temp()}/beans_mmap_example.dat"
    fs.write_bytes(p, Bytes.new(64)).expect("seed")

    let m: MMap = MMap.open(p, true).expect("open rw")
    io.println("{m.len()}")
    m.put_u32(0, 4096).put_u64(8, 123456789).put_u8(16, 255)
    io.println("{m.get_u32(0)} {m.get_u64(8)} {m.get_u8(16)}")
    m.write(20, Bytes.from("hello"))
    io.println(m.read(20, 5).to_string())
    m.flush_range(0, 24).expect("flush_range")
    m.flush().expect("flush")
    m.close().expect("close")
    match m.close() {
        ok(x) => io.println("closed twice?"),
        err(e) => io.println("{e.kind}: {e.msg}"),
    }

    // the writes are durable: a fresh read-only map sees them
    let r: MMap = MMap.open(p, false).expect("open ro")
    io.println("{r.get_u32(0)} {r.read(20, 5).to_string()}")
    match r.flush_range(60, 10) {
        ok(x) => io.println("flushed?"),
        err(e) => io.println("{e.kind}: {e.msg}"),
    }
    match r.resize(16) {
        ok(x) => io.println("resized read-only?"),
        err(e) => io.println("{e.kind}: {e.msg}"),
    }
    match MMap.open("{Dir.temp()}/beans_no_such.dat", false) {
        ok(x) => io.println("opened?"),
        err(e) => io.println("kind {e.kind}"),
    }
    r.close().expect("close ro")

    // resize: grow in place, patch the new tail, shrink back — the handle
    // keeps its fd exactly for this
    fs.write_bytes(p, Bytes.new(8)).expect("seed resize")
    let g: MMap = MMap.open(p, true).expect("open grow")
    g.resize(32).expect("grow")
    g.put_u64(24, 777)
    io.println("{g.len()} {g.get_u64(24)} {File.size(p).expect("grown size")}")
    g.resize(4).expect("shrink")
    io.println("{g.len()} {File.size(p).expect("shrunk size")}")
    g.close().expect("close grow")

    // an empty file maps to len 0; every access is out of range, flush is a no-op
    fs.write(p, "").expect("truncate to empty")
    let z: MMap = MMap.open(p, true).expect("open empty")
    io.println("{z.len()}")
    z.flush().expect("flush empty")
    z.close().expect("close empty")

    // POSIX: the mapping outlives the file — remove the path, keep reading,
    // and the temp dir is already clean when the final panic fires
    fs.write_bytes(p, Bytes.new(8).put_u32(0, 77)).expect("reseed")
    let last: MMap = MMap.open(p, false).expect("open last")
    File.remove(p).expect("rm")
    io.println("{last.get_u32(0)} {File.exists(p)}")
    let boom: int = last.get_u32(6)
    io.println("{boom}")
}
