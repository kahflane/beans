// stdlib phase 3: advisory file locks (flock) — the single-writer database
// pattern. Locks belong to the open file description, so two handles on the
// same file contend: try_lock's ok(false) means "someone else holds it".
import std.io

fn main() {
    let p: string = "{Dir.temp()}/beans_locks_example.dat"
    File.write(p, "guarded").expect("seed")

    let writer: File = File.open(p, "rw").expect("open writer")
    let rival: File = File.open(p, "rw").expect("open rival")

    io.println("{writer.lock().expect("lock")}")
    io.println("{rival.try_lock().expect("try while held")}")
    io.println("{writer.unlock().expect("unlock")}")
    io.println("{rival.try_lock().expect("try after release")}")
    rival.unlock().expect("unlock rival")

    writer.close().expect("close writer")
    match writer.lock() {
        ok(x) => io.println("locked a closed file?"),
        err(e) => io.println("{e.kind}: {e.msg}"),
    }
    rival.close().expect("close rival")

    File.remove(p).expect("cleanup")
    io.println("done")
}
