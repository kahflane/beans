#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "token.h"

namespace beans {

struct LexError {
    std::string msg;
    uint32_t line;
    uint32_t col;
};

// Hand-written scanner. Owns nothing: `source` must outlive the tokens,
// because Token::text is a slice into it.
class Lexer {
public:
    explicit Lexer(std::string_view source);

    // Scan the whole source. Errors don't stop the scan; they are collected
    // and the lexer keeps going so we can report many at once.
    std::vector<Token> scan_all();

    const std::vector<LexError>& errors() const { return errors_; }

private:
    // core cursor
    bool at_end() const { return pos_ >= src_.size(); }
    char peek() const { return at_end() ? '\0' : src_[pos_]; }
    char peek2() const { return pos_ + 1 < src_.size() ? src_[pos_ + 1] : '\0'; }
    char advance();
    bool match(char expected);

    // scanning helpers
    void skip_line_comment();
    bool skip_block_comment();          // false = unterminated
    Token scan_ident_or_keyword();
    Token scan_number();
    Token scan_string();
    bool consume_string_body(int nesting); // after opening quote; false on error

    // token construction
    Token make(TokenKind kind);
    void error_here(std::string msg);
    static bool ends_statement(TokenKind k);

    std::string_view src_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;

    // where the current token started
    size_t start_ = 0;
    uint32_t start_line_ = 1;
    uint32_t start_col_ = 1;

    // last emitted significant token, drives Go-style newline insertion
    TokenKind last_ = TokenKind::newline;
    bool any_token_yet_ = false;

    std::vector<LexError> errors_;
};

} // namespace beans
