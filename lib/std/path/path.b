// Pure path operations in Beans. These functions do not touch the file
// system and use '/' on every supported target.

fn end_without_slashes(value: string) -> int {
    var end: int = value.len()
    for end > 0 && value.byte_at(end - 1) == 47 {
        end -= 1
    }
    return end
}

fn last_slash(value: string, end: int) -> int {
    var index: int = end
    for index > 0 {
        index -= 1
        if value.byte_at(index) == 47 { return index }
    }
    return -1
}

fn base_name(value: string) -> string {
    if value.is_empty() { return "" }
    let end: int = end_without_slashes(value)
    if end == 0 { return "/" }
    let slash: int = last_slash(value, end)
    return value.slice(slash + 1, end)
}

pub fn join(first: string, second: string) -> string {
    if !second.is_empty() && second.byte_at(0) == 47 { return second }
    if first.is_empty() { return second }
    if second.is_empty() { return first }
    let end: int = end_without_slashes(first)
    return "{first.slice(0, end)}/{second}"
}

pub fn parent(value: string) -> string {
    let end: int = end_without_slashes(value)
    if end == 0 {
        if value.is_empty() { return "" }
        return "/"
    }
    let slash: int = last_slash(value, end)
    if slash < 0 { return "" }
    if slash == 0 { return "/" }
    return value.slice(0, slash)
}

pub fn base(value: string) -> string {
    return base_name(value)
}

pub fn ext(value: string) -> string {
    let name: string = base_name(value)
    var index: int = name.len()
    for index > 0 {
        index -= 1
        if name.byte_at(index) == 46 {
            if index == 0 { return "" }
            return name.slice(index, name.len())
        }
    }
    return ""
}

pub fn stem(value: string) -> string {
    let name: string = base_name(value)
    var index: int = name.len()
    for index > 0 {
        index -= 1
        if name.byte_at(index) == 46 {
            if index == 0 { return name }
            return name.slice(0, index)
        }
    }
    return name
}
