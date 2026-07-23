// Bytes growth, unsigned varints, decoding, and CRC32.
import std.io
import std.os
import std.bytes as byte_algo

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(500_000) } else { 500_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    let data: Bytes = Bytes.new(0)
    data.reserve(n * 3)
    var i: int = 0
    for i < n {
        byte_algo.append_varint(data, ((i * 48271 + seed) % 1_000_003) as u64)
        i += 1
    }
    var pos: int = 0
    var checksum: int = 0
    i = 0
    for i < n {
        let value: u64 = byte_algo.decode_varint_at_or(data, pos, 0)
        checksum += value as int
        pos += byte_algo.varint_size(value)
        i += 1
    }
    io.println("bytes {checksum} {data.len()} {byte_algo.crc32(data)}")
}
