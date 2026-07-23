// a tiny append-only key-value store — the proof the database story holds.
// Records are [u32 klen][u32 vlen][key][value]; last write wins; compact()
// rewrites with the durable-commit pattern: temp file, sync, rename over,
// sync the parent dir.
import std.io
import std.fs

class KV {
    pub dir: string = ""
    pub path: string = ""

    pub static fn open_in(dir: string) -> Result<KV> {
        Dir.make_all(dir)?
        let store: KV = new KV()
        store.dir = dir
        store.path = "{dir}/kv.dat"
        return ok(store)
    }

    pub fn set(key: string, value: string) -> Result<int> {
        var rec: Bytes = new Bytes(8)
        rec.put_u32(0, key.len()).put_u32(4, value.len())
        rec.append_str(key).append_str(value)
        return fs.append_bytes(self.path, rec)
    }

    pub fn get(key: string) -> Result<string> {
        let data: Bytes = fs.read_bytes(self.path)?
        var found: string = ""
        var have: bool = false
        var pos: int = 0
        for pos + 8 <= data.len() {
            let kl: int = data.get_u32(pos)
            let vl: int = data.get_u32(pos + 4)
            // a crash mid-append can leave a torn trailing record; stop at it
            // rather than slicing past the end (append-only-log recovery)
            if pos + 8 + kl + vl > data.len() {
                break
            }
            let k: string = data.slice(pos + 8, pos + 8 + kl).to_string()
            if k == key {
                found = data.slice(pos + 8 + kl, pos + 8 + kl + vl).to_string()
                have = true
            }
            pos = pos + 8 + kl + vl
        }
        if have {
            return ok(found)
        }
        return err("key '{key}' not found")
    }

    pub fn size() -> Result<int> {
        return File.size(self.path)
    }

    // rewrite keeping only the last value per key, then commit durably
    pub fn compact() -> Result<int> {
        let data: Bytes = fs.read_bytes(self.path)?
        var names: List<string> = []
        var latest: Map<string, string> = {}
        var pos: int = 0
        for pos + 8 <= data.len() {
            let kl: int = data.get_u32(pos)
            let vl: int = data.get_u32(pos + 4)
            // torn trailing record from a crash mid-append: stop the scan
            if pos + 8 + kl + vl > data.len() {
                break
            }
            let k: string = data.slice(pos + 8, pos + 8 + kl).to_string()
            let v: string = data.slice(pos + 8 + kl, pos + 8 + kl + vl).to_string()
            if !names.contains(k) {
                names.push(k)
            }
            latest[k] = v
            pos = pos + 8 + kl + vl
        }

        var out: Bytes = new Bytes(0)
        var i: int = 0
        for i < names.len() {
            let k: string = names[i]
            let v: string = latest[k]
            var rec: Bytes = new Bytes(8)
            rec.put_u32(0, k.len()).put_u32(4, v.len())
            rec.append_str(k).append_str(v)
            out.append(rec)
            i += 1
        }

        let tmp: string = "{self.path}.tmp"
        fs.write_bytes(tmp, out)?
        let f: File = File.open(tmp, "rw")?
        f.sync()?
        f.close()?
        File.rename(tmp, self.path)?
        Dir.sync(self.dir)?
        return ok(out.len())
    }
}

fn main() {
    let base: string = "{Dir.temp()}/beans_kv_example"
    Dir.remove_all(base)

    let kv: KV = KV.open_in(base).expect("open")
    kv.set("name", "beans").expect("set")
    kv.set("kind", "language").expect("set")
    kv.set("name", "beans v2").expect("set")
    kv.set("year", "2026").expect("set")

    io.println(kv.get("name").expect("get name"))
    io.println(kv.get("kind").expect("get kind"))
    match kv.get("ghost") {
        ok(v) => io.println("ghost = {v}"),
        err(e) => io.println("miss: {e.msg}"),
    }

    let before: int = kv.size().expect("size")
    let after: int = kv.compact().expect("compact")
    io.println("compacted {before} -> {after}")

    io.println(kv.get("name").expect("still there"))
    io.println(kv.get("year").expect("still there"))

    // simulate a crash mid-append: a full 8-byte header claiming a 100+100
    // byte key/value that never got written. get/compact must treat this torn
    // tail as EOF, not slice past the end and panic.
    var torn: Bytes = new Bytes(8)
    torn.put_u32(0, 100).put_u32(4, 100)
    fs.append_bytes(kv.path, torn).expect("torn append")
    io.println(kv.get("name").expect("survives torn tail"))
    let recovered: int = kv.compact().expect("compact past torn tail")
    io.println("recovered, {recovered} bytes")

    Dir.remove_all(base).expect("cleanup")
    io.println("done")
}
