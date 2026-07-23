import std.io

extern "C" struct Packet {
    tag: u8
    count: u32
    ratio: f32
    live: bool
}

extern "C" struct Link {
    code: u32
    next: RawPtr<u8>
}

extern "C" struct Pair {
    small: u16
    bytes: [u8; 3]
}

extern "C" struct Frame {
    pair: Pair
    values: [u32; 2]
    links: [RawPtr<u8>; 2]
}

fn bumped(value: Packet) -> Packet {
    var result: Packet = value
    result.count += 1
    return result
}

fn main() {
    var first: Packet = Packet { tag: 7, count: 40, ratio: 1.5, live: true }
    let copy: Packet = first
    first.count = 99
    let next: Packet = bumped(copy)
    io.println("struct copy {copy.count} {first.count} next {next.count} eq {copy == next}")

    unsafe {
        let memory: RawPtr<Packet> = RawPtr.alloc(2)
        memory.write(copy)
        memory.offset(1).write(next)
        let records: Slice<Packet> = Slice.from_raw(memory, 2)
        let loaded: Packet = records[1]
        io.println("struct layout {memory.element_size()} {memory.element_align()} raw {loaded.tag} {loaded.count} {loaded.ratio} {loaded.live}")

        let byte: RawPtr<u8> = RawPtr.alloc(1)
        byte.write(77)
        let link_memory: RawPtr<Link> = RawPtr.alloc(1)
        link_memory.write(Link { code: 12, next: byte })
        let link: Link = link_memory.read()
        io.println("struct pointer {link_memory.element_size()} {link_memory.element_align()} {link.code} {link.next.read()}")

        let other_byte: RawPtr<u8> = RawPtr.alloc(1)
        other_byte.write(88)
        let frame: Frame = Frame {
            pair: Pair { small: 513, bytes: [4, 5, 6] },
            values: [1000, 2000],
            links: [byte, other_byte],
        }
        let frame_copy: Frame = frame
        let frame_memory: RawPtr<Frame> = RawPtr.alloc(1)
        frame_memory.write(frame)
        let loaded_frame: Frame = frame_memory.read()
        io.println("struct nested {frame_memory.element_size()} {frame_memory.element_align()} {loaded_frame.pair.small} {loaded_frame.pair.bytes[1]} {loaded_frame.values[1]} {loaded_frame.links[0].read()} {loaded_frame.links[1].read()} eq {loaded_frame == frame_copy}")

        let pointer_slot: RawPtr<RawPtr<u8>> = RawPtr.alloc(1)
        pointer_slot.write(other_byte)
        io.println("pointer pointer {pointer_slot.element_size()} {pointer_slot.element_align()} {pointer_slot.read().read()}")
        pointer_slot.free()
        frame_memory.free()
        other_byte.free()
        link_memory.free()
        byte.free()
        memory.free()
    }
}
