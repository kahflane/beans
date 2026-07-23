fn main() {
    unsafe {
        let storage: RawPtr<u8> = RawPtr.alloc(16)
        let bad: RawPtr<i64> = RawPtr.from_address(storage.address() + (1 as u64))
        bad.atomic_load()
        storage.free()
    }
}
