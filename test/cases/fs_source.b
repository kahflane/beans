import std.fs
import std.io
import std.os

fn main() {
    let root: string = os.args().get(0).expect("root")
    let source: string = "{root}/source.bin"
    let copied: string = "{root}/copied.bin"
    let text: string = "{root}/text.txt"

    let first: Bytes = new Bytes(6).put_u32(0, 0x12345678).put_u16(4, 0xabcd)
    let tail: Bytes = new Bytes(3).put_u8(0, 9).put_u8(1, 8).put_u8(2, 7)
    let source_written: int = fs.write_bytes(source, first).expect("source write")
    let source_appended: int = fs.append_bytes(source, tail).expect("source append")
    let source_data: Bytes = fs.read_bytes(source).expect("source read")
    let copied_count: int = fs.copy(source, copied).expect("copy")
    let copied_data: Bytes = fs.read_bytes(copied).expect("copied read")

    let text_count: int = fs.write(text, "hello").expect("text write")
    let text_append: int = fs.append(text, " world").expect("text append")
    let source_text: string = fs.read(text).expect("source text read")
    io.println("fs bytes {source_written} {source_appended} {source_data == copied_data} {copied_count}")
    io.println("fs text {text_count} {text_append} {source_text}")

    File.remove(text).expect("remove text")
    File.remove(copied).expect("remove copy")
    File.remove(source).expect("remove source")
}
