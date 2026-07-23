// stdlib phase 1: Bytes — the binary workhorse. Every accessor, the builder
// chains a database page would use, and the panic at the end.
import std.io

fn main() {
    // construction
    let b: Bytes = new Bytes(16)
    io.println("{b.len()}")
    let hello: Bytes = Bytes.from("hello")
    io.println("{hello.len()} {hello.get(0)} {hello.get(4)}")

    // single bytes
    b.set(0, 65).set(1, 66).set(2, 300)
    io.println("{b.get(0)} {b.get(1)} {b.get(2)}")

    // fixed widths, little-endian, chained like a page header
    let page: Bytes = new Bytes(64)
    page.put_u8(0, 7).put_u16(1, 65535).put_u32(3, 123456789).put_u64(7, 99).put_i64(15, 0 - 5)
    io.println("{page.get_u8(0)} {page.get_u16(1)} {page.get_u32(3)}")
    io.println("{page.get_u64(7)} {page.get_i64(15)}")
    io.println("{page.get_u64(15)}")

    // resize: shrink then regrow reads zeros
    let r: Bytes = new Bytes(4)
    r.fill(255)
    io.println("{r.get(3)}")
    r.resize(2).resize(6)
    io.println("{r.get(0)} {r.get(2)} {r.get(5)} {r.len()}")

    // slice / copy_from / append
    let word: Bytes = Bytes.from("beans language")
    let head: Bytes = word.slice(0, 5)
    io.println(head.to_string())
    let buf: Bytes = new Bytes(5)
    buf.copy_from(head, 0)
    io.println(buf.to_string())
    buf.append(Bytes.from("!")).append_str("!!")
    io.println("{buf.to_string()} {buf.len()}")

    // reserve and record appends grow one unique buffer. append_range copies
    // directly, including when the source and destination are the same value.
    var fast: Bytes = new Bytes(0)
    fast.reserve(64).append_i64(0 - 123456789)
    fast.append_range(Bytes.from("abcd"), 1, 4)
    fast.append_range(fast, 8, 11)
    io.println("{fast.len()} {fast.get_i64(0)} {fast.slice(8, 14).to_string()}")

    // to_string stops at an embedded NUL — strings are text
    let z: Bytes = new Bytes(6)
    z.set(0, 104).set(1, 105)
    io.println("{z.to_string()} {z.to_string().len()}")

    // round-trip
    io.println(Bytes.from("round trip").to_string())

    // varints: unsigned LEB128, negatives move 10 bytes; the size of a value
    // is derivable, so records advance without a second return value
    var vrec: Bytes = new Bytes(0)
    vrec.append_varint(0).append_varint(300).append_varint(-1)
    io.println("{vrec.len()} {Bytes.varint_size(300)} {Bytes.varint_size(-1)}")
    var vpos: int = 0
    var vs: List<int> = []
    for vs.len() < 3 {
        let v: int = vrec.get_varint(vpos)
        vs.push(v)
        vpos = vpos + Bytes.varint_size(v)
    }
    io.println(vs)

    // crc32 (IEEE): the classic check vector, and an empty range
    let chk: Bytes = Bytes.from("123456789")
    io.println("{chk.crc32(0, chk.len())} {chk.crc32(0, 0)}")

    // a u32 read that hangs off the end panics with the position
    let boom: int = page.get_u32(62)
    io.println("{boom}")
}
