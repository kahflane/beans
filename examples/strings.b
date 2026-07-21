// stdlib phase 1: every string method, happy paths and the panic at the end.
// run vs build must be byte-identical.
import std.io

fn main() {
    let s: string = "  Beans Language  "
    let t: string = s.trim()
    io.println(t)
    io.println(s.trim_start())
    io.println(s.trim_end())
    io.println("{t.len()} {s.len()}")
    io.println("{t.is_empty()} {"".is_empty()}")

    io.println(t.to_upper())
    io.println(t.to_lower())
    io.println("{t.first(5)}|{t.last(8)}")
    io.println(t.slice(6, 14))
    io.println("{t.byte_at(0)} {t.byte_at(5)}")

    io.println("{t.contains("Lang")} {t.contains("zzz")}")
    io.println("{t.starts_with("Bean")} {t.starts_with("Lang")}")
    io.println("{t.ends_with("uage")} {t.ends_with("Bean")}")

    match t.find("an") {
        some(i) => io.println("find an: {i}"),
        none => io.println("find an: none"),
    }
    match t.rfind("an") {
        some(i) => io.println("rfind an: {i}"),
        none => io.println("rfind an: none"),
    }
    match t.find("zzz") {
        some(i) => io.println("find zzz: {i}"),
        none => io.println("find zzz: none"),
    }
    match t.find("") {
        some(i) => io.println("find empty: {i}"),
        none => io.println("find empty: none"),
    }
    match t.rfind("") {
        some(i) => io.println("rfind empty: {i}"),
        none => io.println("rfind empty: none"),
    }

    io.println(t.replace("a", "4"))
    io.println(t.replace("zzz", "!"))
    io.println(t.replace("", "!"))
    io.println("ab".repeat(3))
    io.println("x".repeat(0).len())

    let csv: string = "one,two,,four"
    let parts: List<string> = csv.split(",")
    io.println("{parts.len()}")
    io.println("{parts[0]}|{parts[1]}|{parts[2]}|{parts[3]}")
    let whole: List<string> = csv.split(";")
    io.println("{whole.len()} {whole[0]}")
    let onep: List<string> = csv.split("")
    io.println("{onep.len()} {onep[0]}")

    let text: string = "first\nsecond\nthird\n"
    let ls: List<string> = text.lines()
    io.println("{ls.len()}")
    io.println("{ls[0]}|{ls[1]}|{ls[2]}")
    let ls2: List<string> = "no newline".lines()
    io.println("{ls2.len()} {ls2[0]}")

    io.println("{"42".to_int().expect("int")}")
    match "4x2".to_int() {
        ok(v) => io.println("ok {v}"),
        err(e) => io.println("err: {e.msg}"),
    }
    io.println("{"2.5".to_float().expect("float") * 2.0}")
    match "nope".to_float() {
        ok(v) => io.println("ok {v}"),
        err(e) => io.println("err: {e.msg}"),
    }
    let d: decimal = "19.99".to_decimal().expect("decimal")
    io.println("{d + 0.01}")
    match "1.2.3".to_decimal() {
        ok(v) => io.println("ok {v}"),
        err(e) => io.println("err: {e.msg}"),
    }

    // chains
    io.println("  mixed CASE  ".trim().to_lower().replace(" ", "_"))
    let n: int = "  77  ".trim().to_int().expect("n")
    io.println("{n + 1}")

    // out of range slices panic with the range in the message
    io.println(t.slice(5, 99))
}
