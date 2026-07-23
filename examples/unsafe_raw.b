import std.io
import std.thread

fn main() {
    unsafe {
        let empty: RawPtr<u32> = RawPtr.null()
        io.println("null {empty.is_null()}")

        let values: RawPtr<i32> = RawPtr.alloc(4)
        values.write(-7)
        values.offset(1).write(41)
        values.offset(2).write(9)
        values.offset(3).write(12)

        let alias: RawPtr<i32> = RawPtr.from_address(values.address())
        let total: i32 = alias.read() + alias.offset(1).read() +
                         alias.offset(2).read() + alias.offset(3).read()
        io.println("ints {total} same {alias == values}")

        let copied: RawPtr<i32> = RawPtr.alloc(4)
        copied.copy_from(values, 4)
        copied.offset(1).copy_from(copied, 3)
        io.println("layout {copied.element_size()} {copied.element_align()} copy {copied.offset(3).read()}")
        copied.fill_zero(4)
        io.println("zero {copied.offset(1).read()}")

        let small: RawPtr<u8> = RawPtr.alloc(1)
        small.write(255)
        io.println("u8 {small.read()}")

        let fraction: RawPtr<f32> = RawPtr.alloc(1)
        fraction.write(1.25)
        let flag: RawPtr<bool> = RawPtr.alloc(1)
        flag.write_volatile(true)
        io.println("scalar {fraction.read()} {flag.read_volatile()}")

        let atomic: RawPtr<i64> = RawPtr.alloc(1)
        atomic.atomic_store(10)
        let old: i64 = atomic.atomic_fetch_add(5)
        let swapped: bool = atomic.atomic_compare_exchange(15, 20)
        io.println("atomic {old} {swapped} {atomic.atomic_load()}")

        let counter: RawPtr<i64> = RawPtr.alloc(1)
        counter.atomic_store(0)
        let first: Thread<int> = thread.spawn(fn() -> int {
            unsafe {
                var i: int = 0
                for i < 1000 {
                    counter.atomic_fetch_add(1)
                    i += 1
                }
            }
            return 0
        })
        let second: Thread<int> = thread.spawn(fn() -> int {
            unsafe {
                var i: int = 0
                for i < 1000 {
                    counter.atomic_fetch_add(1)
                    i += 1
                }
            }
            return 0
        })
        first.join()
        second.join()
        io.println("parallel atomic {counter.atomic_load()}")

        counter.free()
        atomic.free()
        flag.free()
        fraction.free()
        small.free()
        copied.free()
        values.free()
    }
}
