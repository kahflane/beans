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

/// Join two path segments with a single `/`.
///
/// When to use: building a path from a directory and a name without worrying
/// about duplicate or missing separators. If `second` is absolute (starts with
/// `/`) it is returned as-is; empty segments are skipped.
pub fn join(first: string, second: string) -> string {
    if !second.is_empty() && second.byte_at(0) == 47 { return second }
    if first.is_empty() { return second }
    if second.is_empty() { return first }
    let end: int = end_without_slashes(first)
    return "{first.slice(0, end)}/{second}"
}

/// The parent directory of a path (everything before the last segment).
///
/// When to use: walking up a directory tree. Trailing slashes are ignored;
/// returns `/` for a root-level path and `""` when there is no parent.
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

/// The final segment of a path (the file or directory name).
///
/// When to use: getting a display name from a full path. Trailing slashes are
/// ignored, so `base("/a/b/")` is `"b"`.
pub fn base(value: string) -> string {
    return base_name(value)
}

/// The file extension including the leading dot (e.g. `".txt"`), or `""` if none.
///
/// When to use: branching on file type. A leading dot on the name itself (a
/// dotfile like `.env`) is not treated as an extension.
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

/// The final segment with its extension removed (e.g. `"report"` from
/// `"/a/report.txt"`).
///
/// When to use: deriving an output name from an input file. Complements
/// [ext]: `stem` + `ext` reconstruct [base].
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
