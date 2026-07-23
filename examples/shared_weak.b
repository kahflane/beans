// Explicit shared ownership. Weak observes the value without keeping it alive.
import std.io
import std.thread

class Token {
    value: int

    fn init(value: int) { self.value = value }

    fn deinit() { io.println("drop {self.value}") }
}

fn build_weak() -> Weak<string> {
    let shared: Shared<string> = new Shared("beans")
    let alias: Shared<string> = shared
    let weak: Weak<string> = shared.downgrade()
    io.println("{shared.get()} {alias.get()} {weak.expired()}")
    io.println(weak.upgrade().expect("live").get())
    return weak
}

fn build_dead_token() -> Weak<Token> {
    let shared: Shared<Token> = new Shared(new Token(7))
    return shared.downgrade()
}

fn main() {
    let weak: Weak<string> = build_weak()
    io.println("{weak.expired()} {weak.upgrade().is_none()}")

    let dead: Weak<Token> = build_dead_token()
    io.println("{dead.expired()} {dead.upgrade().is_none()}")

    let amount: Shared<decimal> = new Shared(19.99)
    let amount_weak: Weak<decimal> = amount.downgrade()
    io.println("{amount.get() + 0.01} {amount_weak.upgrade().expect("amount").get()}")

    let number: Shared<int> = new Shared(41)
    let worker: Thread<int> = thread.spawn(fn() -> int { return number.get() + 1 })
    io.println(worker.join())
}
