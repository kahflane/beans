import std.io

@c_layout
struct Packet {
    tag: u8
    count: u32
    ratio: f32
    live: bool
}

@c_layout
union Word {
    bits: u32
    number: f32
}

@c_layout
struct Link {
    code: u32
    next: RawPtr<u8>
}

@c_layout
struct Pair {
    small: u16
    bytes: [u8; 3]
}

@c_layout
union AlignedBlock {
    bytes: [u8; 16]
    word: u64
}

@c_layout
struct Frame {
    pair: Pair
    values: [u32; 2]
    block: AlignedBlock
}

extern "C" fn beans_test_packet_size() -> u64
extern "C" fn beans_test_packet_align() -> u64
extern "C" fn beans_test_packet_offset(index: u64) -> u64
extern "C" fn beans_test_packet_fill(value: RawPtr<Packet>)
extern "C" fn beans_test_packet_roundtrip(value: Packet, extra: u32) -> Packet
extern "C" fn beans_test_word_size() -> u64
extern "C" fn beans_test_word_align() -> u64
extern "C" fn beans_test_word_offset(index: u64) -> u64
extern "C" fn beans_test_word_fill(value: RawPtr<Word>)
extern "C" fn beans_test_word_roundtrip(value: Word) -> Word
extern "C" fn beans_test_link_size() -> u64
extern "C" fn beans_test_link_align() -> u64
extern "C" fn beans_test_link_offset(index: u64) -> u64
extern "C" fn beans_test_link_fill(value: RawPtr<Link>)
extern "C" fn beans_test_pair_size() -> u64
extern "C" fn beans_test_pair_align() -> u64
extern "C" fn beans_test_pair_offset(index: u64) -> u64
extern "C" fn beans_test_block_size() -> u64
extern "C" fn beans_test_block_align() -> u64
extern "C" fn beans_test_block_offset(index: u64) -> u64
extern "C" fn beans_test_frame_size() -> u64
extern "C" fn beans_test_frame_align() -> u64
extern "C" fn beans_test_frame_offset(index: u64) -> u64
extern "C" fn beans_test_frame_fill(value: RawPtr<Frame>)
extern "C" fn beans_test_frame_roundtrip(value: Frame) -> Frame
extern "C" fn beans_test_mixed_float(first: f32, second: f64, third: f32, whole: u64) -> f64

fn main() {
    unsafe {
        let packet: RawPtr<Packet> = RawPtr.alloc(1)
        beans_test_packet_fill(packet)
        let loaded: Packet = packet.read()
        io.println("C struct {beans_test_packet_size()} {beans_test_packet_align()} {beans_test_packet_offset(0)} {beans_test_packet_offset(1)} {beans_test_packet_offset(2)} {beans_test_packet_offset(3)} {loaded.tag} {loaded.count} {loaded.ratio} {loaded.live}")
        let returned: Packet = beans_test_packet_roundtrip(loaded, 7)
        io.println("C struct value {returned.tag} {returned.count} {returned.ratio} {returned.live}")

        let word: RawPtr<Word> = RawPtr.alloc(1)
        beans_test_word_fill(word)
        let loaded_word: Word = word.read()
        io.println("C union {beans_test_word_size()} {beans_test_word_align()} {beans_test_word_offset(0)} {beans_test_word_offset(1)} {loaded_word.bits} {loaded_word.number}")
        let returned_word: Word = beans_test_word_roundtrip(loaded_word)
        io.println("C union value {returned_word.bits} {returned_word.number}")

        let link: RawPtr<Link> = RawPtr.alloc(1)
        beans_test_link_fill(link)
        let loaded_link: Link = link.read()
        io.println("C pointer {beans_test_link_size()} {beans_test_link_align()} {beans_test_link_offset(0)} {beans_test_link_offset(1)} {loaded_link.code} {loaded_link.next.read()}")

        let frame: RawPtr<Frame> = RawPtr.alloc(1)
        beans_test_frame_fill(frame)
        let loaded_frame: Frame = frame.read()
        io.println("C pair {beans_test_pair_size()} {beans_test_pair_align()} {beans_test_pair_offset(0)} {beans_test_pair_offset(1)}")
        io.println("C aligned union {beans_test_block_size()} {beans_test_block_align()} {beans_test_block_offset(0)} {beans_test_block_offset(1)}")
        io.println("C nested {beans_test_frame_size()} {beans_test_frame_align()} {beans_test_frame_offset(0)} {beans_test_frame_offset(1)} {beans_test_frame_offset(2)} Beans {frame.element_size()} {frame.element_align()} {loaded_frame.pair.small} {loaded_frame.pair.bytes[1]} {loaded_frame.values[1]} {loaded_frame.block.word}")
        let returned_frame: Frame = beans_test_frame_roundtrip(loaded_frame)
        io.println("C nested value {returned_frame.pair.small} {returned_frame.values[1]} {returned_frame.block.word}")
        io.println("C mixed float {beans_test_mixed_float(1.25, 2.5, 3.75, 4)}")
        frame.free()
        link.free()
        word.free()
        packet.free()
    }
}
