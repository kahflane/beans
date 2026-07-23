// regress_mem.b — memory-safety regressions from the stdlib-audit pass. The
// program runs clean up to the last line, then panics on the overflow bounds
// check; run-vs-native must match output, panic message, and exit code.
import std.io

// C3: a value read from a container and passed straight into a call used to be
// borrowed, not retained — if the callee overwrote that key, the value's memory
// was freed and reused while the caller still held it (silent wrong answer /
// heap-use-after-free natively). It must now stay alive across the call.
fn clobber(m: Map<string, string>, victim: string) -> string {
    m["k"] = "overwritten-with-a-fresh-string"
    return victim
}

fn uaf_check() {
    var m: Map<string, string> = {}
    m["k"] = "original-value-here"
    // m["k"] is read and handed to clobber, which overwrites m["k"] mid-call
    io.println(clobber(m, m["k"]))
}

// A match field can skip its safety pin only when no arm can name the field's
// owner. This arm replaces `n.next`, so the old Option must keep its child
// alive until the arm is finished reading it.
class PinNode {
    value: int = 0
    next: Option<PinNode> = none
}

fn match_pin_check() {
    let n: PinNode = PinNode { next: some(PinNode { value: 42 }) }
    var got: int = -1
    match n.next {
        some(child) => {
            n.next = none
            got = child.value
        }
        none => { }
    }
    io.println("pin {got}")
}

// C2: bounds checks used to be `pos + width > len` in signed 64-bit, so a huge
// position wrapped negative and sailed past the guard into a wild read/write.
// A legitimate out-of-range access still panics; the overflow one must too.
fn bounds_check() {
    let b: Bytes = Bytes.new(8)
    b.put_u64(0, 42)
    io.println("{b.get_u64(0)}")           // in range: 42
    io.println("{b.get_u64(9223372036854775800)}") // overflow: must panic, not read garbage
}

fn main() {
    uaf_check()
    match_pin_check()
    bounds_check()
    io.println("unreachable")
}
