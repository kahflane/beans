// package money — exact decimal money math

pub class Money {
    pub amount: decimal
    pub currency: string = "USD"

    pub fn new(amount: decimal) -> Money {
        return Money { amount: amount }
    }

    pub fn add(self, other: Money) -> Money {
        return Money { amount: self.amount + other.amount, currency: self.currency }
    }

    pub fn show(self) -> string {
        return "{self.currency} {self.amount}"
    }
}

pub enum Payment {
    cash
    card(number: string)
}

pub fn total(xs: List<Money>) -> Money {
    var sum: decimal = 0.00
    for m: Money in xs {
        sum = sum + m.amount
    }
    return Money.new(sum)
}
