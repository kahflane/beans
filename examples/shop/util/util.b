// package util — shared helpers for the shop
import std.io

pub interface Device {
    fn label(self) -> string

    // default method: runs for any Device, even ones from other packages
    fn describe(self) -> string {
        return "device: {self.label()}"
    }
}

pub class Logger {
    pub prefix: string = "[shop]"

    pub fn log(self, msg: string) {
        io.println("{self.prefix} {msg}")
    }
}

pub fn largest<T: Order>(xs: List<T>) -> Option<T> {
    return xs.max()
}

pub fn tau() -> f64 {
    return 6.2831853
}

pub enum Level {
    low
    high
}

// private — visible inside util only; importing packages can't call it
fn hidden() -> int {
    return 42
}
