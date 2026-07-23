// Binary algorithms in Beans. Bytes allocation, indexed access, and copying
// stay primitive runtime operations; policies and formats live here.

pub fn crc32(data: Bytes) -> u32 {
    var crc: u32 = 0xffffffff
    var index: int = 0
    for index < data.len() {
        crc = crc ^ (data.get(index) as u32)
        // CRC32 always takes eight steps per byte. Writing them out removes a
        // small source-level loop that LLVM did not reliably unroll for Beans.
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        crc = (crc >> 1) ^ (0xedb88320 & ((0 as u32) - (crc & 1)))
        index += 1
    }
    return crc ^ 0xffffffff
}

pub fn varint_size(value: u64) -> int {
    var remaining: u64 = value
    var size: int = 1
    for remaining >= 128 {
        remaining = remaining >> 7
        size += 1
    }
    return size
}

pub fn encode_varint(value: u64) -> Bytes {
    var out: Bytes = Bytes.new(10)
    var remaining: u64 = value
    var index: int = 0
    for remaining >= 128 {
        out.set(index, ((remaining & 0x7f) | 0x80) as int)
        remaining = remaining >> 7
        index += 1
    }
    out.set(index, remaining as int)
    return out.slice(0, index + 1)
}

pub fn append_varint(data: Bytes, value: u64) {
    var remaining: u64 = value
    for {
        var byte: u64 = remaining & 0x7f
        remaining = remaining >> 7
        if remaining != 0 { byte = byte | 0x80 }
        data.push(byte as int)
        if remaining == 0 { return }
    }
}

pub fn decode_varint(data: Bytes) -> Option<u64> {
    var result: u64 = 0
    var shift: u64 = 0
    var index: int = 0
    for index < data.len() && index < 10 {
        let byte: u64 = data.get(index) as u64
        if shift == 63 && (byte & 0xfe) != 0 { return none }
        result = result | ((byte & 0x7f) << shift)
        if (byte & 0x80) == 0 { return some(result) }
        shift += 7
        index += 1
    }
    return none
}

pub fn decode_varint_at_or(data: Bytes, start: int, fallback: u64) -> u64 {
    if start < 0 || start >= data.len() { return fallback }
    var result: u64 = 0
    var shift: u64 = 0
    var index: int = start
    let limit: int = if data.len() < start + 10 { data.len() } else { start + 10 }
    for index < limit {
        let byte: u64 = data.get(index) as u64
        if shift == 63 && (byte & 0xfe) != 0 { return fallback }
        result = result | ((byte & 0x7f) << shift)
        if (byte & 0x80) == 0 { return result }
        shift += 7
        index += 1
    }
    return fallback
}
