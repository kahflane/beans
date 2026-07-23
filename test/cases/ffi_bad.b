struct Plain {
    value: i32
}

extern "C" fn llabs(value: i64) -> i64
extern "C" fn bad_text(value: string) -> string
extern "C" fn bad_plain(value: Plain) -> Plain
extern "C" fn bad_owned(take value: i64) -> i64
extern "C" fn bad_generic<T>(value: i64) -> i64
extern "C" fn bad_callback(callback: fn(string) -> i32) -> i32
extern "C" fn main()

fn main() {
    let value: i64 = llabs(-1)
    let function: fn(i64) -> i64 = llabs
}
