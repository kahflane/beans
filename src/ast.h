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
    enum class Kind { named, fn };
    Kind kind = Kind::named;
    uint32_t line = 0, col = 0;

    // named: Name or Name<args>
    std::string name;
    std::vector<TypePtr> args;

    // fn(params) -> ret
    std::vector<TypePtr> fn_params;
    TypePtr fn_ret; // null = no return value
};

struct Param {
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
};

struct MatchArm {
    PatPtr pat;
    ExprPtr value;
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
    std::string name;
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
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<std::string> supers;
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

// ast_print.cpp — human-readable tree dump
std::string dump(const Module& m);

} // namespace beans
