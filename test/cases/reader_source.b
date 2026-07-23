import std.fs
import std.io
import std.os
import std.reader

fn line(value: Result<Option<string>>) -> string {
    return value.expect("read line").or("<eof>")
}

fn main() {
    let root: string = os.args().get(0).expect("root")
    let path: string = "{root}/lines.txt"
    let nul_path: string = "{root}/nul-lines.txt"
    let long: string = "x".repeat(9000)
    fs.write(path, "alpha\n\n{long}\nlast").expect("seed")

    let source_file: File = File.open(path, "r").expect("open")
    let source: reader.Reader = new reader.Reader(source_file)
    let first: string = line(source.read_line())
    let empty: string = line(source.read_line())
    let long_line: string = line(source.read_line())
    let last: string = line(source.read_line())
    let eof: string = line(source.read_line())
    let equal: bool = first == "alpha" && empty == "" && long_line == long &&
        last == "last" && eof == "<eof>"
    let total: int = first.len() + empty.len() + long_line.len() + last.len() + eof.len()
    source_file.close().expect("close")

    let nul_data: Bytes = new Bytes(5).set(0, 65).set(1, 0).set(2, 66).set(3, 10).set(4, 67)
    fs.write_bytes(nul_path, nul_data).expect("nul seed")
    let source_nul_file: File = File.open(nul_path, "r").expect("nul open")
    let source_nul: reader.Reader = new reader.Reader(source_nul_file)
    let source_first: string = line(source_nul.read_line())
    let source_last: string = line(source_nul.read_line())
    let nul_equal: bool = source_first.len() == 3 && source_first.byte_at(1) == 0 &&
        source_last == "C"
    source_nul_file.close().expect("nul close")

    io.println("reader source {equal} {nul_equal} {total}")
    File.remove(path).expect("remove")
    File.remove(nul_path).expect("remove nul")
}
