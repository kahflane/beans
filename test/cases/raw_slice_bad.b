fn outside(view: Slice<i32>, pointer: RawPtr<i32>) -> i32 {
    let built: Slice<i32> = Slice.from_raw(pointer, 2)
    view.set(0, 1)
    view.subslice(0, 1)
    for value: i32 in view { value }
    return view[0]
}

fn main() {
    let invalid: Slice<string> = Slice.from_raw(RawPtr.null(), 0)
    let nested: List<Slice<i32>> = []
}
