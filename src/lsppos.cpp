#include "lsppos.h"

namespace beans {

namespace {

// Bytes and UTF-16 code units consumed by the UTF-8 sequence starting at a
// lead byte. Malformed lead bytes count as one byte / one unit so a bad file
// can never desynchronize the mapping.
struct Step {
    unsigned bytes;
    unsigned units;
};

Step utf8_step(unsigned char lead) {
    if (lead < 0x80) return {1, 1}; // ASCII
    if (lead < 0xC0) return {1, 1}; // stray continuation byte
    if (lead < 0xE0) return {2, 1}; // 2-byte, BMP
    if (lead < 0xF0) return {3, 1}; // 3-byte, BMP
    return {4, 2};                  // 4-byte, astral -> surrogate pair
}

// Byte offset of the start of a 1-based line.
size_t line_start(std::string_view src, uint32_t line) {
    if (line <= 1) return 0;
    uint32_t seen = 1;
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == '\n') {
            if (++seen == line) return i + 1;
        }
    }
    return src.size();
}

} // namespace

size_t byte_offset(std::string_view src, uint32_t line, uint32_t col) {
    size_t start = line_start(src, line);
    size_t off = start + (col > 0 ? col - 1 : 0);
    return off > src.size() ? src.size() : off;
}

LspPos to_lsp(std::string_view src, uint32_t line, uint32_t col) {
    size_t start = line_start(src, line);
    size_t target = start + (col > 0 ? col - 1 : 0);
    if (target > src.size()) target = src.size();

    uint32_t units = 0;
    size_t i = start;
    while (i < target) {
        if (src[i] == '\n') break; // column ran past the line; stop
        Step s = utf8_step(static_cast<unsigned char>(src[i]));
        if (i + s.bytes > target) break; // cursor lands mid-sequence
        units += s.units;
        i += s.bytes;
    }
    return {line > 0 ? line - 1 : 0, units};
}

void from_lsp(std::string_view src, LspPos p, uint32_t& line, uint32_t& col) {
    size_t start = line_start(src, p.line + 1);
    uint32_t units = 0;
    size_t i = start;
    while (i < src.size() && src[i] != '\n' && units < p.character) {
        Step s = utf8_step(static_cast<unsigned char>(src[i]));
        units += s.units;
        i += s.bytes;
    }
    line = p.line + 1;
    col = static_cast<uint32_t>(i - start) + 1;
}

} // namespace beans
