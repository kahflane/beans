// package util — shared helpers for the shop
import std.io

pub interface Device {
    fn label() -> string

    // default method: runs for any Device, even ones from other packages
    fn describe() -> string {
        return "device: {self.label()}"
    }
}

pub class Logger {
    pub prefix: string = "[shop]"

    pub fn log(msg: string) {
        io.println("{self.prefix} {msg}")
    }
}

pub fn largest<T implements Order>(xs: List<T>) -> Option<T> {
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
