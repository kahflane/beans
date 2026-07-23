// High-level Option combinators written in Beans. The two variants and their
// null-niche layout remain compiler core; algorithms do not need C++ rows.

pub fn map<T: Clone, U>(value: Option<T>, apply: fn(T) -> U) -> Option<U> {
    return match value {
        some(inner) => some(apply(inner)),
        none => none,
    }
}

pub fn and_then<T: Clone, U>(value: Option<T>, apply: fn(T) -> Option<U>) -> Option<U> {
    return match value {
        some(inner) => apply(inner),
        none => none,
    }
}

pub fn filter<T: Clone>(value: Option<T>, keep: fn(T) -> bool) -> Option<T> {
    return match value {
        some(inner) => if keep(inner) { some(inner) } else { none },
        none => none,
    }
}
