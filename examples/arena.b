import std.io

fn main() {
    var words: Arena<string> = Arena.new(2)
    let first: int = words.put("bean")
    let second: int = words.put("sprout")
    io.println("arena {first} {second} {words.len()} {words.get(first)}")
    words.clear()
    io.println("clear {words.len()} {words.get(first)}")

    let moved: Arena<string> = take words
    words = Arena.new(1)
    words.put("new")
    io.println("moved {moved.len()} new {words.get(0)}")
}
