// beans multi-file demo: packages, pub, cross-package everything
import std.io
import shop.money
import shop.util as u

// implementing an interface from another package
class Till : u.Device {
    id: int = 7

    fn label(self) -> string {
        return "till-{self.id}"
    }
}

fn main() {
    banner("beans shop")

    let log: u.Logger = {}
    log.log("open")

    var cart: Cart = {}
    cart.add(money.Money.new(19.99))
    cart.add(money.Money.new(0.01))
    io.println(cart.total().show())

    let m: money.Money = money.Money.new(5.00).add(money.Money.new(2.50))
    io.println(m.show())

    let p: money.Payment = money.Payment.card("4242")
    match p {
        cash => io.println("paid cash"),
        card(n) => {
            // block-bodied match arm: several statements, no value
            let masked: string = "**** {n}"
            io.println("paid by card {masked}")
        }
    }

    let till: Till = {}
    io.println(till.describe())
    let d: u.Device = till
    io.println("as a device: {d.label()}")

    let xs: List<int> = [3, 9, 4]
    match u.largest(xs) {
        some(n) => io.println("largest {n}"),
        none => io.println("empty"),
    }

    io.println("tau {u.tau()}")

    let lv: u.Level = u.Level.high
    match lv {
        low => io.println("level low"),
        high => io.println("level high"),
    }
}
