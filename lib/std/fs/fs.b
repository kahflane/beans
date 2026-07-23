// High-level file helpers in Beans. File.open and positional/cursor I/O stay
// native because they are the syscall boundary.

pub fn read_bytes(path: string) -> Result<Bytes> {
    let file: File = File.open(path, "r")?
    defer file.close()
    let size: int = file.size()?
    return file.read_at(0, size)
}

pub fn read(path: string) -> Result<string> {
    let data: Bytes = read_bytes(path)?
    return ok(data.to_string_full())
}

pub fn write_bytes(path: string, data: Bytes) -> Result<int> {
    let file: File = File.open(path, "create")?
    defer file.close()
    file.truncate(0)?
    return file.write_at(0, data)
}

pub fn append_bytes(path: string, data: Bytes) -> Result<int> {
    let file: File = File.open(path, "append")?
    defer file.close()
    return file.write(data)
}

pub fn write(path: string, data: string) -> Result<int> {
    return write_bytes(path, Bytes.from(data))
}

pub fn append(path: string, data: string) -> Result<int> {
    return append_bytes(path, Bytes.from(data))
}

pub fn copy(from: string, to: string) -> Result<int> {
    let data: Bytes = read_bytes(from)?
    return write_bytes(to, data)
}
