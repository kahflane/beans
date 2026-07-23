#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace beans {

// The beans compiler uses 1-based lines and 1-based *byte* columns. LSP uses
// 0-based lines and 0-based *UTF-16 code unit* character offsets. These helpers
// convert between the two against a file's source text. Kept dependency-free so
// they can be unit-tested on their own.

struct LspPos {
    uint32_t line = 0;      // 0-based
    uint32_t character = 0; // 0-based, UTF-16 code units
};

// Byte offset into `src` of a 1-based line + 1-based byte column. Clamps past
// the end of the line / file rather than reading out of bounds.
size_t byte_offset(std::string_view src, uint32_t line, uint32_t col);

// beans (1-based line, 1-based byte col) -> LSP (0-based line, UTF-16 char).
LspPos to_lsp(std::string_view src, uint32_t line, uint32_t col);

// LSP (0-based line, UTF-16 char) -> beans (1-based line, 1-based byte col).
void from_lsp(std::string_view src, LspPos p, uint32_t& line, uint32_t& col);

} // namespace beans
