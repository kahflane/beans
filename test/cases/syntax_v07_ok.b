import std.io

interface Named {
    fn name() -> string
}

interface Tagged extends Named {
    fn tag() -> string { return "tag:{self.name()}" }
}

interface Counted {
    fn count() -> int { return 1 }
}

class Base {
    value: int

    fn init(value: int) { self.value = value }
    fn read() -> int { return self.value }
    static fn zero() -> Base { return new Base(0) }
}

class Child extends Base implements Tagged, Counted {
    label: string = "child"

    fn init(label: string, value: int) {
        self.label = label
        super.init(value)
    }

    fn name() -> string { return self.label }
}

class GrandChild extends Base {
    extra: int = 2
}

class Defaults {
    value: int = 7
}

pub unique class Handle {
    id: int = 9
}

pub extern "C" struct Pair {
    left: i32
    right: i32
}

fn label_of<T implements Named & Clone>(value: T) -> string {
    return value.name()
}

fn main() {
    let child: Child = new Child("beans", 4)
    let inherited: GrandChild = new GrandChild(5)
    let defaults: Defaults = new Defaults()
    let zero: Base = Base.zero()
    var handle: Handle = new Handle()
    let moved: Handle = move handle
    let pair: Pair = Pair { left: 3, right: 6 }
    io.println("{label_of(child)} {child.tag()} {child.count()} {child.read()} {inherited.read() + inherited.extra} {defaults.value} {zero.read()} {moved.id} {pair.right}")
}
