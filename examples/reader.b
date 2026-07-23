// Beans-written std.reader.Reader — buffered lines over a File. It reads at its
// own offset (pread), so the file's cursor never moves; buffered data keeps
// serving after close, and the closed error surfaces on the next refill.
import std.io
import std.fs
import std.reader

fn main() {
    let p: string = "{Dir.temp()}/beans_reader_example.txt"
    fs.write(p, "alpha\nbeta\n\ngamma with spaces\nlast no newline").expect("seed")

    let f: File = File.open(p, "r").expect("open")
    let r: reader.Reader = reader.Reader(f)
    var n: int = 0
    var stop: bool = false
    for !stop {
        match r.read_line().expect("line") {
            some(line) => {
                io.println("{n}: [{line}]")
                n += 1
            },
            none => {
                stop = true
            },
        }
    }
    io.println("lines: {n}")

    // a second reader starts from the top, and the cursor never moved
    let again: reader.Reader = reader.Reader(f)
    io.println(again.read_line().expect("first again").or("?"))
    io.println("{f.tell()}")
    f.close().expect("close")

    // a file bigger than the 8KB buffer: lines keep serving from the buffer
    // after close, then the refill reports the closed file
    var big: string = ""
    var i: int = 0
    for i < 1200 {
        big = "{big}row number {i}\n"
        i += 1
    }
    fs.write(p, big).expect("big")
    let fb: File = File.open(p, "r").expect("open big")
    let rb: reader.Reader = reader.Reader(fb)
    io.println(rb.read_line().expect("big first").or("?"))
    fb.close().expect("close big")
    var served: int = 0
    var done: bool = false
    for !done {
        match rb.read_line() {
            ok(o) => {
                served += 1
            },
            err(e) => {
                io.println("served {served} buffered, then: {e.kind}: {e.msg}")
                done = true
            },
        }
    }

    // empty file: none straight away
    fs.write(p, "").expect("empty")
    let fe: File = File.open(p, "r").expect("open empty")
    match reader.Reader(fe).read_line().expect("eof") {
        some(x) => io.println("line in empty?"),
        none => io.println("empty: none"),
    }
    fe.close().expect("close empty")
    File.remove(p).expect("rm")
    io.println("done")
}
