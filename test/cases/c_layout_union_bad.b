extern "C" union Empty {}

extern "C" union TextField {
    text: string
}

extern "C" union Generic<T> {
    value: i32
}

extern "C" union DefaultField {
    value: i32 = 1
}

extern "C" union WithMethod {
    value: i32
    fn get() -> i32 { return self.value }
}

extern "C" union Word {
    bits: u32
    number: f32
}

fn main() {
    let outside: Word = Word { bits: 1 }
    outside.bits
    unsafe {
        let empty: Word = Word {}
        let many: Word = Word { bits: 1, number: 1.0 }
        var changed: Word = Word { bits: 2 }
        changed.bits += 1
    }
}
