// tour.b — one file that shows every beans idea so far

import std.io

interface Shape {
    fn area(self) -> f64

    // default method body — most "abstract class" jobs die here
    fn describe(self) -> string {
        return "shape with area {self.area()}"
    }
}

class Circle : Shape {
    r: f64

    fn new(r: f64) -> Circle {
        return Circle { r: r }
    }

    fn area(self) -> f64 {
        return 3.14159265 * self.r * self.r
    }
}

class LoudCircle : Circle {
    override fn describe(self) -> string {
        return "A CIRCLE. AREA {self.area()}."
    }
}

class User {
    name: string
    age: int = 0

    fn new(name: string, age: int) -> User {
        return User { name: name, age: age }
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

    fn push(self, x: T) { self.items.push(x) }
    fn pop(self) -> Option<T> { return self.items.pop() }
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
    let shapes: List<Shape> = [Circle.new(1.0), LoudCircle { r: 2.0 }]
    for s: Shape in shapes {
        io.println(s.describe())
    }

    // as? — checked downcast, returns Option, never crashes
    let first: Shape = Circle.new(1.0)
    match first as? LoudCircle {
        some(lc) => io.println("loud: {lc.describe()}"),
        none     => io.println("just a normal circle"),
    }

    // option / result
    let users: List<User> = [User.new("jul", 30), User.new("mia", 25)]

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
    var st: Stack<int> = {}
    st.push(1)
    st.push(2)
    io.println(st.pop().or(-1))      // 2
}
