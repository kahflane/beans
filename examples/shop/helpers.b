// same package as main.b — no import needed between files of one package
import std.io
import shop.money

fn banner(title: string) {
    io.println("== {title} ==")
}

class Cart {
    items: List<money.Money> = []

    fn add(m: money.Money) {
        self.items.push(m)
    }

    fn total() -> money.Money {
        return money.total(self.items)
    }
}
