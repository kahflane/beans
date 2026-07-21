#include "lexer.h"

#include <utility>

namespace beans {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_ident_char(char c) { return is_alpha(c) || is_digit(c); }

} // namespace

Lexer::Lexer(std::string_view source) : src_(source) {}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') {
        line_ += 1;
        col_ = 1;
    } else {
        col_ += 1;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (at_end() || src_[pos_] != expected) return false;
    advance();
    return true;
}

Token Lexer::make(TokenKind kind) {
    Token t;
    t.kind = kind;
    t.text = src_.substr(start_, pos_ - start_);
    t.line = start_line_;
    t.col = start_col_;
    last_ = kind;
    any_token_yet_ = true;
    return t;
}

void Lexer::error_here(std::string msg) {
    errors_.push_back({std::move(msg), start_line_, start_col_});
}

// Which tokens can be the last thing in a statement? A newline right after
// one of these terminates the statement (same idea as Go's semicolon rule).
bool Lexer::ends_statement(TokenKind k) {
    switch (k) {
        case TokenKind::ident:
        case TokenKind::int_lit:
        case TokenKind::float_lit:
        case TokenKind::string_lit:
        case TokenKind::kw_return:
        case TokenKind::kw_break:
        case TokenKind::kw_continue:
        case TokenKind::kw_self:
        case TokenKind::kw_true:
        case TokenKind::kw_false:
        case TokenKind::rparen:
        case TokenKind::rbracket:
        case TokenKind::rbrace:
        case TokenKind::question:
            return true;
        default:
            return false;
    }
}

void Lexer::skip_line_comment() {
    // stop at the newline, don't eat it — the main loop decides what it means
    while (!at_end() && peek() != '\n') advance();
}

bool Lexer::skip_block_comment() {
    // pos_ is just past the opening "/*". Nesting allowed.
    int depth = 1;
    while (!at_end()) {
        if (peek() == '/' && peek2() == '*') {
            advance(); advance();
            depth += 1;
        } else if (peek() == '*' && peek2() == '/') {
            advance(); advance();
            depth -= 1;
            if (depth == 0) return true;
        } else {
            advance();
        }
    }
    return false;
}

Token Lexer::scan_ident_or_keyword() {
    while (is_ident_char(peek())) advance();
    std::string_view word = src_.substr(start_, pos_ - start_);
    return make(keyword_or_ident(word));
}

Token Lexer::scan_number() {
    bool is_float = false;

    if (peek() == '0' && (peek2() == 'x' || peek2() == 'X')) {
        advance(); advance();
        if (!is_hex(peek())) {
            error_here("hex literal needs at least one digit after 0x");
        }
        while (is_hex(peek()) || peek() == '_') advance();
        return make(TokenKind::int_lit);
    }

    if (peek() == '0' && (peek2() == 'b' || peek2() == 'B')) {
        advance(); advance();
        if (peek() != '0' && peek() != '1') {
            error_here("binary literal needs at least one digit after 0b");
        }
        while (peek() == '0' || peek() == '1' || peek() == '_') advance();
        return make(TokenKind::int_lit);
    }

    while (is_digit(peek()) || peek() == '_') advance();

    // fractional part — but "0..10" is a range, not a float
    if (peek() == '.' && is_digit(peek2())) {
        is_float = true;
        advance(); // '.'
        while (is_digit(peek()) || peek() == '_') advance();
    }

    // exponent: 1e9, 2.5e-3
    if (peek() == 'e' || peek() == 'E') {
        char after = peek2();
        bool signed_exp = (after == '+' || after == '-') &&
                          pos_ + 2 < src_.size() && is_digit(src_[pos_ + 2]);
        if (is_digit(after) || signed_exp) {
            is_float = true;
            advance(); // e
            if (peek() == '+' || peek() == '-') advance();
            while (is_digit(peek())) advance();
        }
    }

    return make(is_float ? TokenKind::float_lit : TokenKind::int_lit);
}

// Consumes a full string body after its opening quote, including the closing
// quote. Understands escapes and {interpolation}, and interpolated expressions
// may themselves contain strings — handled by recursion.
bool Lexer::consume_string_body(int nesting) {
    if (nesting > 16) {
        error_here("string interpolation nested too deep");
        return false;
    }

    int brace_depth = 0;
    while (true) {
        if (at_end()) {
            error_here("string never closed");
            return false;
        }
        char c = peek();

        if (c == '\n') {
            error_here("string not closed before end of line");
            return false; // leave the newline for the main loop
        }

        if (c == '\\') {
            advance();
            if (at_end()) {
                error_here("string never closed");
                return false;
            }
            char e = peek();
            if (e != 'n' && e != 't' && e != 'r' && e != '0' &&
                e != '\\' && e != '"' && e != '{' && e != '}') {
                errors_.push_back({std::string("unknown escape \\") + e, line_, col_});
            }
            advance();
            continue;
        }

        if (c == '{') { brace_depth += 1; advance(); continue; }
        if (c == '}') {
            if (brace_depth > 0) brace_depth -= 1;
            advance();
            continue;
        }

        if (c == '"') {
            advance(); // consume the quote
            if (brace_depth == 0) return true; // our closing quote
            // a nested string inside an interpolated expression
            if (!consume_string_body(nesting + 1)) return false;
            continue;
        }

        advance();
    }
}

Token Lexer::scan_string() {
    advance(); // opening quote
    consume_string_body(0); // on error, we still emit what we got
    return make(TokenKind::string_lit);
}

std::vector<Token> Lexer::scan_all() {
    std::vector<Token> out;

    while (true) {
        // skip whitespace and comments; emit newline tokens where they matter
        bool scanning_trivia = true;
        while (scanning_trivia && !at_end()) {
            char c = peek();
            switch (c) {
                case ' ':
                case '\t':
                case '\r':
                    advance();
                    break;
                case '\n': {
                    uint32_t nl_line = line_;
                    uint32_t nl_col = col_;
                    advance();
                    if (any_token_yet_ && ends_statement(last_)) {
                        Token t;
                        t.kind = TokenKind::newline;
                        t.text = std::string_view{};
                        t.line = nl_line;
                        t.col = nl_col;
                        last_ = TokenKind::newline;
                        out.push_back(t);
                    }
                    break;
                }
                case '/':
                    if (peek2() == '/') {
                        advance(); advance();
                        skip_line_comment();
                    } else if (peek2() == '*') {
                        start_ = pos_; start_line_ = line_; start_col_ = col_;
                        advance(); advance();
                        if (!skip_block_comment()) {
                            error_here("block comment never closed");
                        }
                    } else {
                        scanning_trivia = false;
                    }
                    break;
                default:
                    scanning_trivia = false;
                    break;
            }
        }

        if (at_end()) break;

        start_ = pos_;
        start_line_ = line_;
        start_col_ = col_;

        char c = peek();

        if (is_alpha(c)) { out.push_back(scan_ident_or_keyword()); continue; }
        if (is_digit(c)) { out.push_back(scan_number()); continue; }
        if (c == '"')    { out.push_back(scan_string()); continue; }

        advance();
        switch (c) {
            case '(': out.push_back(make(TokenKind::lparen)); break;
            case ')': out.push_back(make(TokenKind::rparen)); break;
            case '{': out.push_back(make(TokenKind::lbrace)); break;
            case '}': out.push_back(make(TokenKind::rbrace)); break;
            case '[': out.push_back(make(TokenKind::lbracket)); break;
            case ']': out.push_back(make(TokenKind::rbracket)); break;
            case ',': out.push_back(make(TokenKind::comma)); break;
            case ':': out.push_back(make(TokenKind::colon)); break;
            case '?': out.push_back(make(TokenKind::question)); break;
            case '^': out.push_back(make(TokenKind::caret)); break;
            case '~': out.push_back(make(TokenKind::tilde)); break;
            case '.':
                if (match('.')) {
                    out.push_back(make(match('=') ? TokenKind::dotdoteq
                                                  : TokenKind::dotdot));
                } else {
                    out.push_back(make(TokenKind::dot));
                }
                break;
            case '+':
                out.push_back(make(match('=') ? TokenKind::plus_eq : TokenKind::plus));
                break;
            case '-':
                if (match('>'))      out.push_back(make(TokenKind::arrow));
                else if (match('=')) out.push_back(make(TokenKind::minus_eq));
                else                 out.push_back(make(TokenKind::minus));
                break;
            case '*':
                out.push_back(make(match('=') ? TokenKind::star_eq : TokenKind::star));
                break;
            case '/':
                out.push_back(make(match('=') ? TokenKind::slash_eq : TokenKind::slash));
                break;
            case '%':
                out.push_back(make(match('=') ? TokenKind::percent_eq : TokenKind::percent));
                break;
            case '=':
                if (match('='))      out.push_back(make(TokenKind::eq));
                else if (match('>')) out.push_back(make(TokenKind::fat_arrow));
                else                 out.push_back(make(TokenKind::assign));
                break;
            case '!':
                out.push_back(make(match('=') ? TokenKind::neq : TokenKind::bang));
                break;
            case '<':
                if (match('='))      out.push_back(make(TokenKind::le));
                else if (match('<')) out.push_back(make(TokenKind::shl));
                else                 out.push_back(make(TokenKind::lt));
                break;
            case '>':
                if (match('='))      out.push_back(make(TokenKind::ge));
                else if (match('>')) out.push_back(make(TokenKind::shr));
                else                 out.push_back(make(TokenKind::gt));
                break;
            case '&':
                out.push_back(make(match('&') ? TokenKind::andand : TokenKind::amp));
                break;
            case '|':
                out.push_back(make(match('|') ? TokenKind::oror : TokenKind::pipe));
                break;
            default:
                error_here(std::string("unexpected character '") + c + "'");
                break;
        }
    }

    // close the last statement if the file doesn't end with a newline
    if (any_token_yet_ && ends_statement(last_)) {
        out.push_back({TokenKind::newline, {}, line_, col_});
    }
    out.push_back({TokenKind::eof, {}, line_, col_});
    return out;
}

std::string_view split_fmt_spec(std::string_view seg, FmtSpec& spec, std::string* err) {
    int depth = 0;
    bool in_str = false;
    size_t colon = std::string_view::npos;
    for (size_t i = 0; i < seg.size(); i++) {
        char c = seg[i];
        if (c == '\\') {
            i++;
            continue;
        }
        if (in_str) {
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            depth--;
        } else if (c == ':' && depth == 0) {
            colon = i;
            break;
        }
    }
    if (colon == std::string_view::npos) return seg;
    std::string_view expr = seg.substr(0, colon);
    std::string_view s = seg.substr(colon + 1);
    FmtSpec out;
    size_t i = 0;
    bool ok = true;
    if (i < s.size() && s[i] == '-') {
        out.left = true;
        i++;
    }
    size_t wdigits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        out.width = out.width * 10 + (s[i] - '0');
        if (out.width > 1000000) ok = false; // an allocation, not a format
        i++;
        wdigits++;
    }
    if (i < s.size() && s[i] == '.') {
        i++;
        size_t pdigits = 0;
        long long p = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            p = p * 10 + (s[i] - '0');
            if (p > 1000) ok = false;
            i++;
            pdigits++;
        }
        if (pdigits == 0) ok = false;
        out.places = p;
    }
    if (i != s.size()) ok = false;             // trailing junk
    if (out.left && wdigits == 0) ok = false;  // '-' needs a width
    if (wdigits == 0 && out.places < 0) ok = false; // empty spec
    if (!ok) {
        if (err) {
            *err = "bad format spec ':" + std::string(s) +
                   "' — use {x:8}, {x:-8}, {x:.2}, or {x:8.2}";
        }
        return expr;
    }
    out.has = true;
    spec = out;
    return expr;
}

} // namespace beans
