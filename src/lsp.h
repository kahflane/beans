#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// Run the language server: JSON-RPC over stdin/stdout until `exit`. Returns the
// process exit code.
int run_lsp_server();

// ---- semantic query facade (for hover, completion, goto) -----------------
// Built from the checked Program's AST. beans requires written types on lets,
// params, fields, and returns, so these resolve from declared types rather than
// full inference — enough for the overwhelming majority of real code.

// A member of a type.
struct MemberInfo {
    std::string name;
    std::string kind;      // "field" | "method" | "variant"
    std::string signature; // rendered, e.g. "fn norm2(self) -> int"
    std::string doc;       // raw /// block (may be empty)
    std::string where;     // "path:line"
};

// Fields + methods of the class/enum named `type_name`, following `extends`,
// plus builtin methods when it names a builtin type. Empty if unknown.
std::vector<MemberInfo> members_of(const Program& prog, const std::string& type_name);

// The declared type name of the expression under the cursor ("" if unknown).
std::string type_at(const Program& prog, const std::string& file,
                    uint32_t line, uint32_t col);

// A name visible at a position, for completion.
struct ScopeName {
    std::string name;
    std::string kind;      // parameter|local|field|function|type|enum|keyword
    std::string signature; // rendered (may be empty)
    std::string doc;       // raw /// block (may be empty)
};
std::vector<ScopeName> scope_at(const Program& prog, const std::string& file,
                                uint32_t line, uint32_t col);

// Signature help for a call/constructor whose argument list holds the cursor.
struct SignatureInfo {
    bool ok = false;
    std::string label;               // e.g. "add(a: int, b: int) -> int"
    std::vector<std::string> params; // per-parameter labels, e.g. "a: int"
    int active = 0;                  // 0-based active parameter
    std::string doc;                 // raw /// block (may be empty)
};
SignatureInfo signature_at(const Program& prog, const std::string& file,
                           uint32_t line, uint32_t col);

} // namespace beans
