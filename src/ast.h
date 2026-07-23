#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "token.h"

namespace beans {

struct TypeRef;
struct Expr;
struct Stmt;
struct Pattern;

using TypePtr = std::unique_ptr<TypeRef>;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using PatPtr = std::unique_ptr<Pattern>;

// ---- types ----------------------------------------------------------------

struct TypeRef {
    enum class Kind { named, fn, fixed_array };
    Kind kind = Kind::named;
    uint32_t line = 0, col = 0;

    // named: Name, pkg.Name, or Name<args>
    std::string name;
    std::vector<TypePtr> args;

    // filled by the checker: package-qualified internal name ("util.User").
    // Later stages (interp, codegen) use it instead of re-resolving imports.
    mutable std::string resolved;

    // fn(params) -> ret
    std::vector<TypePtr> fn_params;
    TypePtr fn_ret; // null = no return value

    // [T; N]
    TypePtr array_elem;
    uint32_t array_len = 0;
};

struct Param {
    enum class Passing { borrow, take, inout };
    Passing passing = Passing::borrow;
    std::string name;
    TypePtr type; // may be null for untyped match bindings
    uint32_t line = 0, col = 0;
};

struct GenericParam {
    std::string name;
    std::string bound; // empty = unbounded
};

// ---- patterns -------------------------------------------------------------

struct Pattern {
    enum class Kind {
        wildcard, // _
        literal,  // 200, "x", true, -1
        range,    // 400..=499
        name,     // variant or binding: cash, some(u), card(n: string)
        alt,      // a | b | c
    };
    Kind kind;
    uint32_t line = 0, col = 0;

    ExprPtr lit;  // literal / range low
    ExprPtr lit2; // range high
    bool inclusive = false;

    std::string name;            // name pattern
    std::vector<Param> bindings; // payload bindings (types optional)
    bool has_payload = false;

    std::vector<PatPtr> alts;
};

// ---- expressions ----------------------------------------------------------

struct MatchArm;

struct InitEntry {
    std::string name; // set for `field: value`
    ExprPtr key;      // set for `expr_key: value` (map literal)
    ExprPtr value;
};

struct Expr {
    enum class Kind {
        int_lit, float_lit, string_lit, bool_lit,
        ident, self_ref,
        unary,  // op rhs
        binary, // lhs op rhs
        range,  // lhs .. rhs / lhs ..= rhs
        call,   // callee(args)
        field,  // object.name
        index,  // object[index_expr]
        list_lit, // [args...]
        init,     // Name<type_args> { entries } | { entries } (short/map)
        cast,     // object as Type / object as? Type
        try_,     // object?
        closure,  // fn(params) -> type { body }
        if_expr,  // if cond { then_e } else { else_e }
        match_expr,
    };
    Kind kind;
    uint32_t line = 0, col = 0;

    std::string_view text; // literal text / ident name
    bool bool_val = false;

    TokenKind op = TokenKind::eof;
    ExprPtr lhs, rhs;
    bool inclusive = false; // range

    ExprPtr callee;
    std::vector<ExprPtr> args; // call args / list elements

    std::string name; // field name / init type name (empty = short init)
    ExprPtr object;
    ExprPtr index_expr;

    std::vector<TypePtr> type_args;
    std::vector<InitEntry> entries;

    TypePtr type;         // cast target / closure return
    bool checked = false; // as? vs as

    std::vector<Param> params;  // closure
    std::vector<StmtPtr> body;  // closure

    ExprPtr cond, then_e, else_e; // if_expr
    ExprPtr subject;              // match_expr
    std::vector<MatchArm> arms;

    // filled by the checker: package-qualified target for names that cross a
    // package line — "util.hello" on a call, "util.User" on an init,
    // "std.io.println" on a std call. Empty = resolve locally as before.
    mutable std::string resolved;

    // filled by the checker on number literals: the type the literal took from
    // its spot (1 = int family, 2 = float, 3 = decimal). 0 = never checked —
    // only re-parsed interpolation segments; interp falls back to runtime
    // hints there. Without this the interpreter guessed from the lexeme and a
    // decimal-typed argument silently became a float Value.
    mutable uint8_t numk = 0;
};

struct MatchArm {
    PatPtr pat;
    ExprPtr value;              // expression arm
    std::vector<StmtPtr> body;  // block arm: pattern => { stmts }
    bool is_block = false;
};

// ---- statements -----------------------------------------------------------

struct Stmt {
    enum class Kind {
        let_,     // let/var name: type = init
        assign,   // target op value
        expr,     // expression statement
        ret, brk, cont,
        if_,      // cond, body, else_body (else-if = single if_ in else_body)
        for_ever, for_while, for_in,
        defer_,
        unsafe_,
    };
    Kind kind;
    uint32_t line = 0, col = 0;

    bool is_var = false;
    std::string name;
    TypePtr type;
    ExprPtr init;

    ExprPtr target;
    TokenKind op = TokenKind::eof;
    ExprPtr value;

    ExprPtr expr; // expr stmt / return value / defer call
    ExprPtr cond;

    std::vector<StmtPtr> body;
    std::vector<StmtPtr> else_body;

    std::string loop_var;
    TypePtr loop_type;
    ExprPtr iterable;
};

// ---- declarations ---------------------------------------------------------

struct FnDecl {
    bool is_pub = false;
    bool is_override = false;
    bool has_self = false;
    bool has_body = true; // false = interface signature
    bool is_extern_c = false;
    std::string extern_name; // unmangled linker symbol
    std::string name;
    std::string qualname; // package-qualified ("util.hello"); loader stamps it,
                          // root package and single files keep the plain name
    std::vector<GenericParam> generics;
    std::vector<Param> params; // not counting self
    TypePtr ret;               // null = no return value
    std::vector<StmtPtr> body;
    uint32_t line = 0, col = 0;
};

struct FieldDecl {
    bool is_pub = false;
    std::string name;
    TypePtr type;
    ExprPtr def; // default value, may be null
    uint32_t line = 0, col = 0;
};

struct ClassDecl { // also interfaces (is_interface)
    bool is_pub = false;
    bool is_interface = false;
    bool is_struct = false;   // inline value, never an ARC object
    bool is_union = false;    // inline overlapping value, unsafe field access
    bool is_c_layout = false; // target C field order/alignment is part of its ABI
    bool is_move_only = false;
    std::string name;
    std::string qualname;
    std::vector<GenericParam> generics;
    std::vector<std::string> supers;
    // checker fills: supers as package-qualified names, same order as supers
    mutable std::vector<std::string> supers_resolved;
    std::vector<FieldDecl> fields;
    std::vector<FnDecl> methods;
    uint32_t line = 0, col = 0;
};

struct EnumVariant {
    std::string name;
    std::vector<Param> payload;
};

struct EnumDecl {
    bool is_pub = false;
    std::string name;
    std::string qualname;
    std::vector<GenericParam> generics;
    std::vector<EnumVariant> variants;
    std::vector<FnDecl> methods;
    uint32_t line = 0, col = 0;
};

struct ImportDecl {
    std::string path;  // verbatim: std.io or github.com/acme/http
    std::string alias; // empty = last segment
    uint32_t line = 0, col = 0;
};

struct Module {
    std::vector<ImportDecl> imports;
    std::vector<ClassDecl> classes;
    std::vector<EnumDecl> enums;
    std::vector<FnDecl> fns;

    // original declaration order for printing: (kind, index into vector above)
    enum class DeclKind { class_, enum_, fn };
    std::vector<std::pair<DeclKind, size_t>> order;
};

// ---- whole programs (loader output) ----------------------------------------

// One parsed .b file. Owns its source text — the AST's string_views point into
// it, so PFiles live on the heap and never move.
struct PFile {
    std::string path;
    std::string source;
    Module mod;
};

// One package = one directory of .b files sharing a namespace.
struct Package {
    std::string prefix;      // qualifier for decls ("util"); root package = ""
    std::string import_path; // how imports name it ("shop.util", "github.com/x/y")
    std::string dir;
    std::vector<std::unique_ptr<PFile>> files;
};

struct Program {
    std::string module_name; // from beans.mod; empty = single-file mode
    std::vector<std::unique_ptr<Package>> packages; // dependencies first, root last

    const Package* root() const { return packages.back().get(); }
};

// ast_print.cpp — human-readable tree dump
std::string dump(const Module& m);

} // namespace beans
