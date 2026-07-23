import std.io

extern "C" union Word {
    bits: u32
    number: f32
}

extern "C" union AlignedBlock {
    bytes: [u8; 16]
    word: u64
}

fn passthrough(value: Word) -> Word {
    return value
}

fn main() {
    unsafe {
        var word: Word = Word { bits: 1065353216 }
        io.println("union bits {word.bits} number {word.number}")
        word.number = 2.5
        let copy: Word = passthrough(word)
        io.println("union write {copy.bits} number {copy.number}")

        let memory: RawPtr<Word> = RawPtr.alloc(1)
        memory.write(copy)
        let loaded: Word = memory.read()
        io.println("union layout {memory.element_size()} {memory.element_align()} raw {loaded.number}")
        memory.free()

        let block: AlignedBlock = AlignedBlock {
            bytes: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16],
        }
        io.println("union aligned {block.word}")
        let block_memory: RawPtr<AlignedBlock> = RawPtr.alloc(1)
        block_memory.write(block)
        io.println("union aligned layout {block_memory.element_size()} {block_memory.element_align()} {block_memory.read().bytes[15]}")
        block_memory.free()
    }
}
