import std.io

fn find(keys: List<int>, key: int) -> int {
    var index: int = 0
    for index < keys.len() {
        if keys.get(index).or(-1) == key { return index }
        index += 1
    }
    return -1
}

fn main() {
    var plain: Map<int, int> = {}
    var ordered: OrderedMap<int, int> = {}
    plain.reserve(256)
    ordered.reserve(256)

    var model_keys: List<int> = []
    var model_values: List<int> = []
    var seed: u64 = 0x123456789abcdef0
    var errors: int = 0
    var step: int = 0
    for step < 5000 {
        seed = seed * 6364136223846793005 + 1442695040888963407
        let key: int = (seed % 193) as int
        let at: int = find(model_keys, key)
        let operation: int = step % 6
        if operation < 3 {
            let value: int = ((seed >> 17) as int) ^ step
            plain[key] = value
            ordered[key] = value
            if at < 0 {
                model_keys.push(key)
                model_values.push(value)
            } else {
                model_values.remove(at)
                model_values.insert(at, value)
            }
        } else if operation == 3 {
            let value: int = ((seed >> 17) as int) ^ step
            let expected: bool = at < 0
            if plain.insert(key, value) != expected { errors += 1 }
            if ordered.insert(key, value) != expected { errors += 1 }
            if expected {
                model_keys.push(key)
                model_values.push(value)
            }
        } else if operation == 4 {
            let expected: bool = at >= 0
            if plain.remove(key) != expected { errors += 1 }
            if ordered.remove(key) != expected { errors += 1 }
            if expected {
                model_keys.remove(at)
                model_values.remove(at)
            }
        } else {
            let expected: int = if at < 0 { -1 } else { model_values.get(at).or(-1) }
            if plain.get(key).or(-1) != expected { errors += 1 }
            if ordered.get(key).or(-1) != expected { errors += 1 }
            if plain.contains(key) != (at >= 0) { errors += 1 }
            if ordered.contains(key) != (at >= 0) { errors += 1 }
        }
        if plain.len() != model_keys.len() { errors += 1 }
        if ordered.len() != model_keys.len() { errors += 1 }
        step += 1
    }

    let ordered_keys: List<int> = ordered.keys()
    var index: int = 0
    var checksum: int = 0
    for index < model_keys.len() {
        let key: int = model_keys.get(index).or(-1)
        let value: int = model_values.get(index).or(-1)
        if ordered_keys.get(index).or(-1) != key { errors += 1 }
        if plain.get(key).or(-1) != value { errors += 1 }
        if ordered.get(key).or(-1) != value { errors += 1 }
        checksum = checksum ^ (key * 65537) ^ value
        index += 1
    }

    let plain_keys: List<int> = plain.keys()
    let plain_values: List<int> = plain.values()
    var seen: Map<int, bool> = {}
    seen.reserve(256)
    var value_sum: int = 0
    for key: int in plain_keys {
        if !seen.insert(key, true) { errors += 1 }
        if !plain.contains(key) { errors += 1 }
    }
    for value: int in plain_values { value_sum += value }
    var model_sum: int = 0
    for value: int in model_values { model_sum += value }
    if value_sum != model_sum { errors += 1 }
    if seen.len() != plain.len() { errors += 1 }

    io.println("map model {errors} {plain.len()} {checksum} {model_sum}")
}
