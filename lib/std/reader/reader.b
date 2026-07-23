// Buffered line reading in Beans. File.read_at remains the native syscall
// primitive; buffering, offsets, newline scanning, and EOF policy live here.

pub class Reader {
    file: File
    buffer: Bytes
    position: int
    limit: int
    offset: int
    eof: bool

    pub fn init(file: File) {
        self.file = file
        self.buffer = new Bytes(0)
        self.position = 0
        self.limit = 0
        self.offset = 0
        self.eof = false
    }

    pub fn read_line() -> Result<Option<string>> {
        var output: Bytes = new Bytes(0)
        for true {
            if self.position == self.limit {
                if self.eof {
                    if output.len() == 0 { return ok(none) }
                    return ok(some(output.to_string_full()))
                }
                let next: Bytes = self.file.read_at(self.offset, 8192)?
                if next.len() == 0 {
                    self.eof = true
                    if output.len() == 0 { return ok(none) }
                    return ok(some(output.to_string_full()))
                }
                self.offset += next.len()
                self.position = 0
                self.limit = next.len()
                self.buffer = move next
            }

            var end: int = self.position
            for end < self.limit && self.buffer.get(end) != 10 { end += 1 }
            output.append(self.buffer.slice(self.position, end))
            self.position = end
            if end < self.limit {
                self.position += 1
                return ok(some(output.to_string_full()))
            }
        }
        return ok(none)
    }
}
