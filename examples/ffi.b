import std.io

extern "C" fn llabs(value: i64) -> i64
extern "C" fn memset(dest: RawPtr<u8>, value: i32, count: u64) -> RawPtr<u8>
extern "C" fn fabs(value: f64) -> f64
extern "C" fn fabsf(value: f32) -> f32
extern "C" fn ldexp(value: f64, exponent: i32) -> f64
extern "C" fn ldexpf(value: f32, exponent: i32) -> f32

fn main() {
    unsafe {
        let memory: RawPtr<u8> = RawPtr.alloc(4)
        let returned: RawPtr<u8> = memset(memory, 65, 4)
        let checksum: int = (returned.read() as int) +
                            (returned.offset(1).read() as int) +
                            (returned.offset(2).read() as int) +
                            (returned.offset(3).read() as int)
        io.println("ffi {llabs(-42)} {checksum} {returned == memory}")
        io.println("float {fabs(-3.5)} {fabsf(-2.25)}")
        io.println("mixed {ldexp(1.5, 3)} {ldexpf(3.0, -1)}")
        memory.free()
    }
}
