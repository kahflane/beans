import std.io

fn read(value: Box<int>) -> int {
    return value.get()
}

fn main() {
    var value: Box<int> = Box.new(7)
    io.println("box {read(value)}")
    value.set(11)

    let moved: Box<int> = take value
    value = Box.new(13)
    io.println("moved {moved.get()} new {value.get()}")

    let text: Box<string> = Box.new("bean")
    let copy: string = text.get()
    text.set("beans")
    io.println("text {copy} {text.get()}")
}
