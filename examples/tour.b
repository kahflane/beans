// tour.b — one file that shows every beans idea so far

import std.io

interface Shape {
    fn area() -> f64

    // default method body — most "abstract class" jobs die here
    fn describe() -> string {
        return "shape with area {self.area()}"
    }
}

class Circle implements Shape {
    r: f64

    fn init(r: f64) {
        self.r = r
    }

    fn area() -> f64 {
        return 3.14159265 * self.r * self.r
    }
}

class LoudCircle extends Circle {
    override fn describe() -> string {
        return "A CIRCLE. AREA {self.area()}."
    }
}

class User {
    name: string
    age: int = 0

    fn init(name: string, age: int) {
        self.name = name
        self.age = age
    }
}

// enums: snake_case variants, payloads allowed, built for match
enum Payment {
    cash
    card(number: string)
    transfer(iban: string, amount: decimal)
}

fn describe_payment(p: Payment) -> string {
    return match p {
        cash => "paid cash",
        card(n) => "card ending {n.last(4)}",
        transfer(iban, amt) => "sent {amt} to {iban}",
    }
}

// can fail -> says so in the type. no exceptions anywhere.
fn parse_age(s: string) -> Result<int> {
    let n: int = s.to_int()?     // err? pass it up. ok? unwrap.
    if n < 0 {
        return err("negative age")
    }
    return ok(n)
}

// might be missing -> Option. no null anywhere.
fn find(users: List<User>, name: string) -> Option<User> {
    for u: User in users {
        if u.name == name {
            return some(u)
        }
    }
    return none
}

// generics: monomorphized, zero-cost
class Stack<T> {
    items: List<T> = []

    fn push(x: T) { self.items.push(x) }
    fn pop() -> Option<T> { return self.items.pop() }
}

fn main() {
    // everything is an object
    io.println((-5).abs())           // 5
    io.println("42".to_int().or(0))  // 42

    // decimal: exact money math. floats can't do this.
    let price: decimal = 19.99
    let qty: int = 3
    let total: decimal = price * (qty as decimal)
    io.println("total: {total}")     // total: 59.97, exactly

    // shapes + inheritance + override
    let shapes: List<Shape> = [new Circle(1.0), new LoudCircle(2.0)]
    for s: Shape in shapes {
        io.println(s.describe())
    }

    // as? — checked downcast, returns Option, never crashes
    let first: Shape = new Circle(1.0)
    match first as? LoudCircle {
        some(lc) => io.println("loud: {lc.describe()}"),
        none     => io.println("just a normal circle"),
    }

    // option / result
    let users: List<User> = [new User("jul", 30), new User("mia", 25)]

    match find(users, "jul") {
        some(u) => io.println("found {u.name}, age {u.age}"),
        none    => io.println("nobody"),
    }

    match parse_age("abc") {
        ok(n)  => io.println("age {n}"),
        err(e) => io.println("bad input: {e.msg}"),
    }

    // enum in use
    let p: Payment = Payment.transfer("DE89370400440532013000", 250.00)
    io.println(describe_payment(p))

    // if is an expression, one loop keyword
    var i: int = 0
    for i < 3 {
        let kind: string = if i % 2 == 0 { "even" } else { "odd" }
        io.println("{i} is {kind}")
        i += 1
    }

    // generics in use — short init: left side already says the type
    var st: Stack<int> = new Stack()
    st.push(1)
    st.push(2)
    io.println(st.pop().or(-1))      // 2
}
