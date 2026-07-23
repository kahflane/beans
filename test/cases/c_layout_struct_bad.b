struct Empty {}

struct TextField {
    text: string
}

struct Generic<T> {
    value: T
}

struct Plain {
    value: i32
}

@c_layout
struct BadNested {
    value: Plain
}

@c_layout
struct Recursive {
    next: Recursive
}

@c_layout
struct RecursiveArray {
    next: [RecursiveArray; 1]
}

struct Child : Plain {
    other: i32
}

struct WithMethod {
    value: i32
    fn get(self) -> i32 { return self.value }
}

@c_layout
struct Packet {
    value: i32
}

fn main() {
    let frozen: Packet = Packet { value: 1 }
    frozen.value = 2
    let boxed: List<Packet> = []
    unsafe {
        let pointer: RawPtr<Plain> = RawPtr.alloc(1)
        pointer.free()
    }
}
