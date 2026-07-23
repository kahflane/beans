fn read_outside(ptr: RawPtr<i32>) -> i32 {
    return ptr.read()
}

fn simd_outside(left: Simd4f32, right: Simd4f32) -> Simd4f32 {
    left.sum()
    return left + right
}

fn main() {
    let bad_type: RawPtr<string> = RawPtr.null()
    let pointer: RawPtr<i32> = RawPtr.alloc(1)
    pointer.write(4)
    pointer.read_volatile()
    pointer.write_volatile(5)
    pointer.atomic_load()
    pointer.atomic_store(1)
    pointer.atomic_fetch_add(1)
    pointer.atomic_compare_exchange(1, 2)
    pointer.copy_from(pointer, 1)
    pointer.fill_zero(1)
    pointer.element_size()
    pointer.free()
    let vector: Simd4f32 = Simd4f32.splat(1.0)
    let vectors: List<Simd4f32> = []
}
