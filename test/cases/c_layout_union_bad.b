@c_layout
union Empty {}

@c_layout
union TextField {
    text: string
}

@c_layout
union Generic<T> {
    value: i32
}

@c_layout
union DefaultField {
    value: i32 = 1
}

@c_layout
union WithMethod {
    value: i32
    fn get(self) -> i32 { return self.value }
}

@c_layout
union Word {
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
