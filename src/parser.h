#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "token.h"

namespace beans {

struct ParseError {
    std::string msg;
    uint32_t line;
    uint32_t col;
};

// Hand-written recursive descent over the token stream.
// Owns a copy of the tokens because generic-close handling may split a `>>`
// token into two `>` in place.
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    Module parse_module();

    // parse exactly one expression (used for string interpolation segments)
    ExprPtr parse_standalone_expr();

    const std::vector<ParseError>& errors() const { return errors_; }

private:
    // cursor
    const Token& cur() const { return tokens_[pos_]; }
    const Token& next() const { return tokens_[pos_ + 1 < tokens_.size() ? pos_ + 1 : pos_]; }
    bool check(TokenKind k) const { return cur().kind == k; }
    bool at_eof() const { return check(TokenKind::eof); }
    const Token& advance();
    bool accept(TokenKind k);
    bool expect(TokenKind k, const char* what);
    void skip_newlines();
    void end_stmt(); // newline(s), or next is `}` / eof
    void expect_close_angle(); // `>` — splits `>>` in place

    // errors
    void error_at(const Token& t, std::string msg);
    void sync_stmt(); // skip to after newline / before `}`
    void sync_decl(); // skip to next top-level keyword

    // declarations
    ImportDecl parse_import();
    void parse_decl(Module& m);
    ClassDecl parse_class(bool is_pub, bool is_interface, bool is_move_only = false,
                          bool is_struct = false, bool is_c_layout = false,
                          bool is_union = false);
    EnumDecl parse_enum(bool is_pub);
    FnDecl parse_fn(bool is_pub, bool is_override, bool allow_no_body,
                    bool is_method = false, bool is_static = false);
    std::vector<GenericParam> parse_generics();
    std::vector<Param> parse_params();

    // types
    TypePtr parse_type();

    // statements
    std::vector<StmtPtr> parse_block();
    StmtPtr parse_stmt();
    StmtPtr parse_let(bool is_var);
    StmtPtr parse_if_stmt();
    StmtPtr parse_for();

    // expressions (precedence climbing)
    ExprPtr parse_expr();          // range level (lowest)
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_equality();
    ExprPtr parse_comparison();
    ExprPtr parse_bitor();
    ExprPtr parse_bitxor();
    ExprPtr parse_bitand();
    ExprPtr parse_shift();
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_cast();          // x as T / x as? T
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_primary();
    ExprPtr parse_if_expr();
    ExprPtr parse_match_expr();
    ExprPtr parse_closure();
    ExprPtr parse_init_body(std::string type_name,
                            std::vector<TypePtr> type_args,
                            const Token& start); // after `{`
    std::vector<ExprPtr> parse_call_args();      // after `(`

    // patterns
    PatPtr parse_pattern();
    PatPtr parse_pattern_atom();

    // expression parsed inside if/for/match headers must not treat
    // `Name {` as an initializer — the `{` belongs to the body
    bool allow_struct_ = true;
    struct StructGuard {
        Parser& p; bool saved;
        StructGuard(Parser& p, bool v) : p(p), saved(p.allow_struct_) { p.allow_struct_ = v; }
        ~StructGuard() { p.allow_struct_ = saved; }
    };

    std::vector<Token> tokens_;
    size_t pos_ = 0;
    std::vector<ParseError> errors_;
};

} // namespace beans
