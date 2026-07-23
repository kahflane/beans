class Base {
    fn make() -> Base { return new Base() }
    static fn answer() -> int { return 42 }
}
class Child extends Base {}
fn main() {
    let made: Base = Base.make()
    let inherited: int = Child.answer()
}
