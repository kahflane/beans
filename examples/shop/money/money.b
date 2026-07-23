// package money — exact decimal money math

pub class Money {
    pub amount: decimal
    pub currency: string = "USD"

    pub fn init(amount: decimal) {
        self.amount = amount
    }

    pub fn add(other: Money) -> Money {
        let result: Money = new Money(self.amount + other.amount)
        result.currency = self.currency
        return result
    }

    pub fn show() -> string {
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
    return new Money(sum)
}
