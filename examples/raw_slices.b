import std.io

fn sum_view(view: Slice<i32>) -> i32 {
    var total: i32 = 0
    unsafe {
        for value: i32 in view {
            total += value
        }
    }
    return total
}

fn main() {
    unsafe {
        let storage: RawPtr<i32> = RawPtr.alloc(6)
        let all: Slice<i32> = Slice.from_raw(storage, 6)
        var index: int = 0
        for index < all.len() {
            all.set(index, ((index + 1) * 10) as i32)
            index += 1
        }
        let middle: Slice<i32> = all.subslice(1, 5)
        middle.set(1, 99)
        io.println("slice {middle.len()} {middle[0]} {middle.get(2)} {sum_view(middle)}")
        io.println("slice ptr {middle.as_ptr() == storage.offset(1)} tail {all[5]}")
        storage.free()
    }
}
