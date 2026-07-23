extern "C" fn beans_test_call_once(callback: fn() -> i32) -> i32

fn main() {
    unsafe {
        let missing: RawPtr<i32> = RawPtr.null()
        beans_test_call_once(fn() -> i32 { return missing.read() })
    }
}
