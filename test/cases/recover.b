// A half-typed member access (as during editing). The parser must report the
// error but still recover: the statement that follows must survive so the rest
// of the file keeps parsing (completion needs the receiver `u` captured too).
fn main() {
    let u: string = "hi"
    u.
    let z: int = 5
}
