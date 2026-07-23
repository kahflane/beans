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

extern "C" struct BadNested {
    value: Plain
}

extern "C" struct Recursive {
    next: Recursive
}

extern "C" struct RecursiveArray {
    next: [RecursiveArray; 1]
}

struct Child extends Plain {
    other: i32
}

struct WithMethod {
    value: i32
    fn get() -> i32 { return self.value }
}

extern "C" struct Packet {
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
