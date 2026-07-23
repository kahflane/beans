import std.io

fn multiply_add(value: Simd4f32, scale: Simd4f32) -> Simd4f32 {
    unsafe {
        return value * scale + value
    }
}

fn main() {
    unsafe {
        let source: Simd4f32 = Simd4f32.of(1.0, 2.0, 3.0, 4.0)
        let scale: Simd4f32 = Simd4f32.splat(2.0)
        let result: Simd4f32 = multiply_add(source, scale)
        io.println("simd {result.lane(0)} {result.lane(1)} {result.lane(2)} {result.lane(3)} sum {result.sum()}")

        let memory: RawPtr<f32> = RawPtr.alloc(4)
        result.store(memory)
        let loaded: Simd4f32 = Simd4f32.load(memory)
        io.println("load {loaded.lane(2)} layout {memory.element_size()} {memory.element_align()}")
        memory.free()
    }
}
