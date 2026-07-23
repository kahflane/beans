#pragma once

#include <cstdint>
#include <string>

#include "ast.h"

namespace beans {

// The Markdown an editor hover would show for the symbol whose name sits under
// (line, col) — 1-based, byte columns, matching the lexer — inside an
// already-loaded Program. Empty string = nothing resolvable there.
//
// This is deliberately name-based for now: it finds the identifier under the
// cursor and looks it up across the program's declarations and the builtin
// registry. It does not yet use the checker's per-expression resolution, so a
// name that exists in several places resolves to the first match. That upgrade
// (a real symbol-at-cursor over the resolved AST) is the next step; the CLI
// contract here stays the same.
std::string hover_at(const Program& prog, const std::string& file,
                     uint32_t line, uint32_t col);

} // namespace beans
