// stdlib phase 3: advisory file locks (flock) — the single-writer database
// pattern. Locks belong to the open file description, so two handles on the
// same file contend: try_lock's ok(false) means "someone else holds it".
//
// This example uses try_lock so its output stays deterministic. The blocking
// lock() waits until the holder releases (across processes or threads that
// each opened the file separately) — it retries through EINTR. A single thread
// that calls lock() on a description it already holds via another handle would
// wait forever, so use try_lock when the same thread might already hold it.
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
