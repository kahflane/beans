// High-level collection algorithms live in Beans. List/Map storage and their
// primitive operations are still runtime intrinsics while the buffer types
// are being moved into core.

pub fn count_int(values: List<int>, needle: int) -> int {
    var count: int = 0
    for value: int in values {
        if value == needle {
            count += 1
        }
    }
    return count
}

pub fn sum_int(values: List<int>) -> int {
    var total: int = 0
    for value: int in values {
        total += value
    }
    return total
}

pub fn frequencies(values: List<string>) -> Map<string, int> {
    var counts: Map<string, int> = {}
    counts.reserve(values.len())
    for value: string in values {
        match counts.get(value) {
            some(count) => { counts[value] = count + 1 }
            none => { counts[value] = 1 }
        }
    }
    return move counts
}

pub fn unique(values: List<string>) -> List<string> {
    var seen: Map<string, bool> = {}
    var result: List<string> = []
    seen.reserve(values.len())
    result.reserve(values.len())
    for value: string in values {
        if !seen.contains(value) {
            seen[value] = true
            result.push(value)
        }
    }
    return move result
}

pub fn count<T implements Eq>(values: List<T>, needle: T) -> int {
    var total: int = 0
    for value: T in values {
        if value == needle { total += 1 }
    }
    return total
}

pub fn filter<T implements Clone>(values: List<T>, keep: fn(T) -> bool) -> List<T> {
    var result: List<T> = []
    result.reserve(values.len())
    for value: T in values {
        if keep(value) { result.push(value) }
    }
    return move result
}

pub fn transform<T implements Clone, U>(values: List<T>, apply: fn(T) -> U) -> List<U> {
    var result: List<U> = []
    result.reserve(values.len())
    for value: T in values {
        result.push(apply(value))
    }
    return move result
}

pub fn unique_of<T implements Eq & Hash & Clone>(values: List<T>) -> List<T> {
    var seen: Map<T, bool> = {}
    var result: List<T> = []
    seen.reserve(values.len())
    result.reserve(values.len())
    for value: T in values {
        if !seen.contains(value) {
            seen[value] = true
            result.push(value)
        }
    }
    return move result
}

// These Map policies are ordinary Beans code. The native boundary only owns
// hashing, probing, and typed storage; adding another policy does not need a
// checker, interpreter, codegen, or C-runtime special case.

pub fn increment<K implements Eq & Hash>(inout values: Map<K, int>, key: K, delta: int) -> int {
    let next: int = match values.get(key) {
        some(current) => current + delta,
        none => delta,
    }
    values[key] = next
    return next
}

pub fn get_or_insert<K implements Eq & Hash, V implements Clone>(
    inout values: Map<K, V>, key: K, make: fn() -> V) -> V {
    match values.get(key) {
        some(current) => { return current }
        none => {}
    }
    let fresh: V = make()
    values[key] = fresh
    return fresh
}

pub fn merge_with<K implements Eq & Hash & Clone, V implements Clone>(
    inout target: Map<K, V>, source: Map<K, V>,
    combine: fn(V, V) -> V) {
    let keys: List<K> = source.keys()
    for key: K in keys {
        let incoming: V = source[key]
        match target.get(key) {
            some(current) => { target[key] = combine(current, incoming) }
            none => { target[key] = incoming }
        }
    }
}

pub fn remove_if<K implements Eq & Hash & Clone, V implements Clone>(
    inout values: Map<K, V>, remove: fn(K, V) -> bool) -> int {
    let keys: List<K> = values.keys()
    var removed: int = 0
    for key: K in keys {
        match values.get(key) {
            some(value) => {
                if remove(key, value) && values.remove(key) { removed += 1 }
            }
            none => {}
        }
    }
    return removed
}

pub fn map_values<K implements Eq & Hash & Clone, V implements Clone, U>(
    values: Map<K, V>, apply: fn(K, V) -> U) -> Map<K, U> {
    var result: Map<K, U> = {}
    result.reserve(values.len())
    let keys: List<K> = values.keys()
    for key: K in keys {
        result[key] = apply(key, values[key])
    }
    return move result
}
