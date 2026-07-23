// High-level Result combinators written in Beans. Result layout, propagation,
// and Error construction stay compiler core; policies do not need C++ rows.

pub fn map<T: Clone, U>(value: Result<T>, apply: fn(T) -> U) -> Result<U> {
    return match value {
        ok(inner) => ok(apply(inner)),
        err(error) => err(error),
    }
}

pub fn and_then<T: Clone, U>(value: Result<T>, apply: fn(T) -> Result<U>) -> Result<U> {
    return match value {
        ok(inner) => apply(inner),
        err(error) => err(error),
    }
}

pub fn recover<T: Clone>(value: Result<T>, fallback: fn(Error) -> T) -> T {
    return match value {
        ok(inner) => inner,
        err(error) => fallback(error),
    }
}
