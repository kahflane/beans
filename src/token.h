#pragma once

#include <cstdint>
#include <string_view>

namespace beans {

enum class TokenKind : uint8_t {
    // names and literals
    ident,
    int_lit,
    float_lit,
    string_lit,

    // tokens with reserved keyword spellings (33; `unique` is contextual)
    kw_class, kw_struct, kw_union, kw_interface, kw_enum, kw_fn, kw_let, kw_var, kw_pub, kw_override,
    kw_if, kw_else, kw_for, kw_in, kw_match, kw_return, kw_break, kw_continue,
    kw_import, kw_as, kw_defer, kw_unsafe, kw_extern, kw_new,
    kw_extends, kw_implements, kw_static, kw_move, kw_take, kw_inout,
    kw_self, kw_true, kw_false,

    // brackets
    lparen, rparen, lbrace, rbrace, lbracket, rbracket,

    // punctuation
    comma, semicolon, dot, dotdot, dotdoteq, colon, question, at,
    arrow,      // ->
    fat_arrow,  // =>

    // operators
    plus, minus, star, slash, percent,
    assign,     // =
    plus_eq, minus_eq, star_eq, slash_eq, percent_eq,
    eq, neq, lt, le, gt, ge,
    andand, oror, bang,
    amp, pipe, caret, tilde, shl, shr,

    // structure
    newline,    // inserted Go-style: only after a token that can end a statement
    eof,
};

struct Token {
    TokenKind kind;
    std::string_view text;   // slice into the source buffer
    std::string_view doc;    // `///` doc block bound to this token, else empty
    uint32_t line;           // 1-based
    uint32_t col;            // 1-based, bytes
};

// keyword lookup: returns the keyword kind, or ident if not a keyword
TokenKind keyword_or_ident(std::string_view word);

// stable name for printing/debugging
const char* to_string(TokenKind k);

} // namespace beans
