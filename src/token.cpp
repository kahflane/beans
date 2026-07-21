#include "token.h"

namespace beans {

TokenKind keyword_or_ident(std::string_view w) {
    struct Entry { std::string_view word; TokenKind kind; };
    static const Entry table[] = {
        {"class", TokenKind::kw_class},
        {"interface", TokenKind::kw_interface},
        {"enum", TokenKind::kw_enum},
        {"fn", TokenKind::kw_fn},
        {"let", TokenKind::kw_let},
        {"var", TokenKind::kw_var},
        {"pub", TokenKind::kw_pub},
        {"override", TokenKind::kw_override},
        {"if", TokenKind::kw_if},
        {"else", TokenKind::kw_else},
        {"for", TokenKind::kw_for},
        {"in", TokenKind::kw_in},
        {"match", TokenKind::kw_match},
        {"return", TokenKind::kw_return},
        {"break", TokenKind::kw_break},
        {"continue", TokenKind::kw_continue},
        {"import", TokenKind::kw_import},
        {"as", TokenKind::kw_as},
        {"defer", TokenKind::kw_defer},
        {"unsafe", TokenKind::kw_unsafe},
        {"self", TokenKind::kw_self},
        {"true", TokenKind::kw_true},
        {"false", TokenKind::kw_false},
    };
    for (const Entry& e : table) {
        if (e.word == w) return e.kind;
    }
    return TokenKind::ident;
}

const char* to_string(TokenKind k) {
    switch (k) {
        case TokenKind::ident:       return "ident";
        case TokenKind::int_lit:     return "int";
        case TokenKind::float_lit:   return "float";
        case TokenKind::string_lit:  return "string";
        case TokenKind::kw_class:    return "class";
        case TokenKind::kw_interface:return "interface";
        case TokenKind::kw_enum:     return "enum";
        case TokenKind::kw_fn:       return "fn";
        case TokenKind::kw_let:      return "let";
        case TokenKind::kw_var:      return "var";
        case TokenKind::kw_pub:      return "pub";
        case TokenKind::kw_override: return "override";
        case TokenKind::kw_if:       return "if";
        case TokenKind::kw_else:     return "else";
        case TokenKind::kw_for:      return "for";
        case TokenKind::kw_in:       return "in";
        case TokenKind::kw_match:    return "match";
        case TokenKind::kw_return:   return "return";
        case TokenKind::kw_break:    return "break";
        case TokenKind::kw_continue: return "continue";
        case TokenKind::kw_import:   return "import";
        case TokenKind::kw_as:       return "as";
        case TokenKind::kw_defer:    return "defer";
        case TokenKind::kw_unsafe:   return "unsafe";
        case TokenKind::kw_self:     return "self";
        case TokenKind::kw_true:     return "true";
        case TokenKind::kw_false:    return "false";
        case TokenKind::lparen:      return "(";
        case TokenKind::rparen:      return ")";
        case TokenKind::lbrace:      return "{";
        case TokenKind::rbrace:      return "}";
        case TokenKind::lbracket:    return "[";
        case TokenKind::rbracket:    return "]";
        case TokenKind::comma:       return ",";
        case TokenKind::dot:         return ".";
        case TokenKind::dotdot:      return "..";
        case TokenKind::dotdoteq:    return "..=";
        case TokenKind::colon:       return ":";
        case TokenKind::question:    return "?";
        case TokenKind::arrow:       return "->";
        case TokenKind::fat_arrow:   return "=>";
        case TokenKind::plus:        return "+";
        case TokenKind::minus:       return "-";
        case TokenKind::star:        return "*";
        case TokenKind::slash:       return "/";
        case TokenKind::percent:     return "%";
        case TokenKind::assign:      return "=";
        case TokenKind::plus_eq:     return "+=";
        case TokenKind::minus_eq:    return "-=";
        case TokenKind::star_eq:     return "*=";
        case TokenKind::slash_eq:    return "/=";
        case TokenKind::percent_eq:  return "%=";
        case TokenKind::eq:          return "==";
        case TokenKind::neq:         return "!=";
        case TokenKind::lt:          return "<";
        case TokenKind::le:          return "<=";
        case TokenKind::gt:          return ">";
        case TokenKind::ge:          return ">=";
        case TokenKind::andand:      return "&&";
        case TokenKind::oror:        return "||";
        case TokenKind::bang:        return "!";
        case TokenKind::amp:         return "&";
        case TokenKind::pipe:        return "|";
        case TokenKind::caret:       return "^";
        case TokenKind::tilde:       return "~";
        case TokenKind::shl:         return "<<";
        case TokenKind::shr:         return ">>";
        case TokenKind::newline:     return "newline";
        case TokenKind::eof:         return "eof";
    }
    return "?";
}

} // namespace beans
