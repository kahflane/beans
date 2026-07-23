#include "parser.h"

#include <cstdlib>
#include <utility>

namespace beans {

namespace {

ExprPtr new_expr(Expr::Kind k, const Token& t) {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->line = t.line;
    e->col = t.col;
    return e;
}

StmtPtr new_stmt(Stmt::Kind k, const Token& t) {
    auto s = std::make_unique<Stmt>();
    s->kind = k;
    s->line = t.line;
    s->col = t.col;
    return s;
}

bool is_assign_op(TokenKind k) {
    switch (k) {
        case TokenKind::assign:
        case TokenKind::plus_eq:
        case TokenKind::minus_eq:
        case TokenKind::star_eq:
        case TokenKind::slash_eq:
        case TokenKind::percent_eq:
            return true;
        default:
            return false;
    }
}

} // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
    if (tokens_.empty()) {
        tokens_.push_back({TokenKind::eof, {}, {}, 1, 1});
    }
}

// ---- cursor ---------------------------------------------------------------

const Token& Parser::advance() {
    const Token& t = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) pos_ += 1;
    return t;
}

bool Parser::accept(TokenKind k) {
    if (check(k)) { advance(); return true; }
    return false;
}

bool Parser::expect(TokenKind k, const char* what) {
    if (accept(k)) return true;
    error_at(cur(), std::string("expected ") + what);
    return false;
}

void Parser::skip_newlines() {
    while (check(TokenKind::newline)) advance();
}

void Parser::end_stmt() {
    if (check(TokenKind::newline)) { skip_newlines(); return; }
    if (check(TokenKind::rbrace) || at_eof()) return;
    error_at(cur(), "expected end of statement");
    sync_stmt();
}

void Parser::expect_close_angle() {
    if (accept(TokenKind::gt)) return;
    if (check(TokenKind::shr)) {
        // split `>>` into two `>` — consume the first half in place
        tokens_[pos_].kind = TokenKind::gt;
        tokens_[pos_].col += 1;
        return;
    }
    error_at(cur(), "expected '>'");
}

// ---- errors ---------------------------------------------------------------

void Parser::error_at(const Token& t, std::string msg) {
    errors_.push_back({std::move(msg), t.line, t.col});
}

void Parser::sync_stmt() {
    while (!at_eof()) {
        if (check(TokenKind::newline)) { skip_newlines(); return; }
        if (check(TokenKind::rbrace)) return;
        advance();
    }
}

void Parser::sync_decl() {
    while (!at_eof()) {
        switch (cur().kind) {
            case TokenKind::kw_class:
            case TokenKind::kw_struct:
            case TokenKind::kw_union:
            case TokenKind::kw_interface:
            case TokenKind::kw_enum:
            case TokenKind::kw_fn:
            case TokenKind::kw_extern:
            case TokenKind::kw_import:
            case TokenKind::kw_pub:
            case TokenKind::at:
                return;
            default:
                advance();
        }
    }
}

// ---- module and declarations ----------------------------------------------

ExprPtr Parser::parse_standalone_expr() {
    skip_newlines();
    ExprPtr e = parse_expr();
    skip_newlines();
    if (!at_eof()) error_at(cur(), "unexpected trailing tokens");
    return e;
}

Module Parser::parse_module() {
    Module m;
    skip_newlines();
    while (!at_eof()) {
        if (check(TokenKind::kw_import)) {
            m.imports.push_back(parse_import());
        } else {
            size_t before = errors_.size();
            parse_decl(m);
            if (errors_.size() > before) sync_decl();
        }
        skip_newlines();
    }
    return m;
}

ImportDecl Parser::parse_import() {
    ImportDecl d;
    const Token& kw = advance(); // import
    d.line = kw.line;
    d.col = kw.col;

    // one path segment; repo names like `beans-test` lex as ident-minus-ident
    auto segment = [&](std::string& out) {
        if (!check(TokenKind::ident)) return false;
        out.append(cur().text);
        advance();
        while (check(TokenKind::minus) && next().kind == TokenKind::ident) {
            advance();
            out.push_back('-');
            out.append(cur().text);
            advance();
        }
        return true;
    };

    std::string path;
    if (!segment(path)) {
        error_at(cur(), "expected import path");
        sync_stmt();
        return d;
    }
    while (check(TokenKind::dot) || check(TokenKind::slash)) {
        path.push_back(check(TokenKind::dot) ? '.' : '/');
        advance();
        if (!segment(path)) {
            error_at(cur(), "expected name after separator in import path");
            break;
        }
    }
    d.path = std::move(path);
    if (accept(TokenKind::kw_as)) {
        if (check(TokenKind::ident)) {
            d.alias = std::string(cur().text);
            advance();
        } else {
            error_at(cur(), "expected name after 'as'");
        }
    }
    end_stmt();
    return d;
}

void Parser::parse_decl(Module& m) {
    std::string doc(cur().doc); // doc block bound to this decl's first token
    bool is_pub = accept(TokenKind::kw_pub);
    if (accept(TokenKind::at)) {
        std::string name = check(TokenKind::ident) ? std::string(cur().text) : "";
        if (check(TokenKind::ident)) advance();
        if (name == "move_only")
            error_at(cur(), "@move_only was removed — use 'unique class'");
        else if (name == "c_layout")
            error_at(cur(), "@c_layout was removed — use 'extern \"C\" struct' or 'extern \"C\" union'");
        else
            error_at(cur(), "attributes are not supported here");
        return;
    }
    bool is_move_only = false;
    if (check(TokenKind::ident) && cur().text == "unique") {
        is_move_only = true;
        advance();
    }
    bool is_c_layout = false;
    if (accept(TokenKind::kw_extern)) {
        is_c_layout = true;
        if (!check(TokenKind::string_lit) || cur().text != "\"C\"")
            error_at(cur(), "expected \"C\" after extern");
        else
            advance();
    }
    switch (cur().kind) {
        case TokenKind::kw_class:
            if (is_c_layout) error_at(cur(), "extern \"C\" applies to structs, unions, and functions, not classes");
            m.classes.push_back(parse_class(is_pub, false, is_move_only));
            m.classes.back().doc = doc;
            m.order.push_back({Module::DeclKind::class_, m.classes.size() - 1});
            break;
        case TokenKind::kw_struct:
            if (is_move_only) error_at(cur(), "unique applies to classes, not structs");
            m.classes.push_back(parse_class(is_pub, false, is_move_only, true, is_c_layout));
            m.classes.back().doc = doc;
            m.order.push_back({Module::DeclKind::class_, m.classes.size() - 1});
            break;
        case TokenKind::kw_union:
            if (is_move_only) error_at(cur(), "unique applies to classes, not unions");
            if (!is_c_layout) error_at(cur(), "union requires extern \"C\"");
            m.classes.push_back(parse_class(is_pub, false, is_move_only, false, true, true));
            m.classes.back().doc = doc;
            m.order.push_back({Module::DeclKind::class_, m.classes.size() - 1});
            break;
        case TokenKind::kw_interface:
            if (is_move_only) error_at(cur(), "unique applies to classes, not interfaces");
            if (is_c_layout) error_at(cur(), "extern \"C\" does not apply to interfaces");
            m.classes.push_back(parse_class(is_pub, true, false));
            m.classes.back().doc = doc;
            m.order.push_back({Module::DeclKind::class_, m.classes.size() - 1});
            break;
        case TokenKind::kw_enum:
            if (is_move_only) error_at(cur(), "unique applies to classes, not enums");
            if (is_c_layout) error_at(cur(), "extern \"C\" does not apply to enums");
            m.enums.push_back(parse_enum(is_pub));
            m.enums.back().doc = doc;
            m.order.push_back({Module::DeclKind::enum_, m.enums.size() - 1});
            break;
        case TokenKind::kw_fn:
            if (is_move_only) error_at(cur(), "unique applies to classes, not functions");
            {
                FnDecl f = parse_fn(is_pub, false, is_c_layout);
                f.doc = doc;
                if (is_c_layout) {
                    f.is_extern_c = true;
                    f.extern_name = f.name;
                    if (f.has_body)
                        error_at(cur(), "extern function is a declaration and has no body");
                }
                m.fns.push_back(std::move(f));
            }
            m.order.push_back({Module::DeclKind::fn, m.fns.size() - 1});
            break;
        default:
            error_at(cur(), "expected class, struct, union, interface, enum, fn, or extern declaration");
            advance();
            break;
    }
}

ClassDecl Parser::parse_class(bool is_pub, bool is_interface, bool is_move_only,
                              bool is_struct, bool is_c_layout, bool is_union) {
    ClassDecl c;
    c.is_pub = is_pub;
    c.is_interface = is_interface;
    c.is_struct = is_struct;
    c.is_union = is_union;
    c.is_c_layout = is_c_layout;
    c.is_move_only = is_move_only;
    const Token& kw = advance(); // class / struct / union / interface
    c.line = kw.line;
    c.col = kw.col;

    if (check(TokenKind::ident)) {
        c.name_line = cur().line;
        c.name_col = cur().col;
        c.name = std::string(cur().text);
        advance();
    } else {
        error_at(cur(), is_interface ? "expected interface name"
                                     : is_union ? "expected union name"
                                     : is_struct ? "expected struct name"
                                                 : "expected class name");
    }
    c.qualname = c.name;
    if (check(TokenKind::lt)) c.generics = parse_generics();

    auto parent_name = [&]() -> std::string {
        if (!check(TokenKind::ident)) {
            error_at(cur(), "expected type name");
            return {};
        }
        std::string name(cur().text);
        advance();
        if (check(TokenKind::dot) && next().kind == TokenKind::ident) {
            advance();
            name += '.';
            name += std::string(cur().text);
            advance();
        }
        return name;
    };

    if (accept(TokenKind::colon)) {
        error_at(cur(), "':' inheritance was removed — use extends and implements");
        do {
            std::string name = parent_name();
            if (name.empty()) break;
            c.interfaces.push_back(std::move(name));
        } while (accept(TokenKind::comma));
    }
    if (accept(TokenKind::kw_extends)) {
        if (is_interface) {
            do {
                std::string name = parent_name();
                if (name.empty()) break;
                c.interfaces.push_back(std::move(name));
            } while (accept(TokenKind::comma));
        } else {
            c.base = parent_name();
        }
    }
    if (accept(TokenKind::kw_implements)) {
        if (is_interface)
            error_at(cur(), "interfaces extend other interfaces; they do not implement them");
        do {
            std::string name = parent_name();
            if (name.empty()) break;
            c.interfaces.push_back(std::move(name));
        } while (accept(TokenKind::comma));
    }

    expect(TokenKind::lbrace, "'{'");
    skip_newlines();
    while (!check(TokenKind::rbrace) && !at_eof()) {
        std::string mdoc(cur().doc); // doc block bound to this member's first token
        bool member_pub = accept(TokenKind::kw_pub);
        bool member_override = accept(TokenKind::kw_override);
        bool member_static = accept(TokenKind::kw_static);
        if (check(TokenKind::kw_fn)) {
            if (is_interface && member_static)
                error_at(cur(), "static interface methods are not supported");
            c.methods.push_back(parse_fn(member_pub, member_override, is_interface,
                                         true, member_static));
            c.methods.back().doc = mdoc;
        } else if (check(TokenKind::ident) && !member_override && !member_static) {
            FieldDecl f;
            f.doc = mdoc;
            f.is_pub = member_pub;
            f.line = cur().line;
            f.col = cur().col;
            f.name_line = cur().line;
            f.name_col = cur().col;
            f.name = std::string(cur().text);
            advance();
            expect(TokenKind::colon, "':' after field name");
            f.type = parse_type();
            if (accept(TokenKind::assign)) f.def = parse_expr();
            end_stmt();
            stamp_end(f);
            c.fields.push_back(std::move(f));
        } else {
            error_at(cur(), "expected field or method");
            sync_stmt();
        }
        skip_newlines();
    }
    expect(TokenKind::rbrace, "'}'");
    stamp_end(c);
    return c;
}

EnumDecl Parser::parse_enum(bool is_pub) {
    EnumDecl e;
    e.is_pub = is_pub;
    const Token& kw = advance(); // enum
    e.line = kw.line;
    e.col = kw.col;

    if (check(TokenKind::ident)) {
        e.name_line = cur().line;
        e.name_col = cur().col;
        e.name = std::string(cur().text);
        advance();
    } else {
        error_at(cur(), "expected enum name");
    }
    e.qualname = e.name;
    if (check(TokenKind::lt)) e.generics = parse_generics();

    expect(TokenKind::lbrace, "'{'");
    skip_newlines();
    while (!check(TokenKind::rbrace) && !at_eof()) {
        std::string mdoc(cur().doc); // doc block bound to this member's first token
        if (check(TokenKind::kw_fn) || check(TokenKind::kw_pub)) {
            bool method_pub = accept(TokenKind::kw_pub);
            e.methods.push_back(parse_fn(method_pub, false, false, true, false));
            e.methods.back().doc = mdoc;
        } else if (check(TokenKind::ident)) {
            EnumVariant v;
            v.doc = mdoc;
            v.line = cur().line;
            v.col = cur().col;
            v.name = std::string(cur().text);
            advance();
            if (accept(TokenKind::lparen)) {
                skip_newlines();
                if (!check(TokenKind::rparen)) {
                    do {
                        skip_newlines();
                        Param p;
                        if (check(TokenKind::ident)) {
                            p.line = cur().line;
                            p.col = cur().col;
                            p.name = std::string(cur().text);
                            advance();
                        } else {
                            error_at(cur(), "expected payload field name");
                            break;
                        }
                        expect(TokenKind::colon, "':' after payload field name");
                        p.type = parse_type();
                        v.payload.push_back(std::move(p));
                        skip_newlines();
                    } while (accept(TokenKind::comma));
                }
                expect(TokenKind::rparen, "')'");
            }
            stamp_end(v);
            accept(TokenKind::comma);
            e.variants.push_back(std::move(v));
        } else {
            error_at(cur(), "expected enum variant or method");
            sync_stmt();
        }
        skip_newlines();
    }
    expect(TokenKind::rbrace, "'}'");
    stamp_end(e);
    return e;
}

FnDecl Parser::parse_fn(bool is_pub, bool is_override, bool allow_no_body,
                        bool is_method, bool is_static) {
    FnDecl f;
    f.is_pub = is_pub;
    f.is_override = is_override;
    f.is_static = is_static;
    f.has_self = is_method && !is_static;
    const Token& kw = advance(); // fn
    f.line = kw.line;
    f.col = kw.col;

    if (check(TokenKind::ident)) {
        f.name_line = cur().line;
        f.name_col = cur().col;
        f.name = std::string(cur().text);
        advance();
    } else if (check(TokenKind::kw_new)) {
        error_at(cur(), "'fn new' was removed — construct with 'new Type(...)' or use a named static");
        f.name = "new";
        advance();
    } else {
        error_at(cur(), "expected function name");
    }
    f.qualname = f.name;
    if (check(TokenKind::lt)) f.generics = parse_generics();

    expect(TokenKind::lparen, "'('");
    f.params = parse_params();
    expect(TokenKind::rparen, "')'");

    if (accept(TokenKind::arrow)) f.ret = parse_type();

    if (check(TokenKind::lbrace)) {
        f.body = parse_block();
        f.has_body = true;
    } else if (allow_no_body) {
        f.has_body = false;
        end_stmt();
    } else {
        error_at(cur(), "expected '{' to start function body");
    }
    stamp_end(f);
    return f;
}

std::vector<GenericParam> Parser::parse_generics() {
    std::vector<GenericParam> out;
    advance(); // <
    do {
        skip_newlines();
        GenericParam g;
        if (check(TokenKind::ident)) {
            g.name = std::string(cur().text);
            advance();
        } else {
            error_at(cur(), "expected type parameter name");
            break;
        }
        if (accept(TokenKind::colon)) {
            error_at(cur(), "':' generic bounds were removed — use implements with '&'");
        }
        if (accept(TokenKind::kw_implements)) {
            do {
                if (!check(TokenKind::ident)) {
                    error_at(cur(), "expected interface after implements");
                    break;
                }
                std::string bound(cur().text);
                advance();
                if (check(TokenKind::dot) && next().kind == TokenKind::ident) {
                    advance();
                    bound += '.';
                    bound += std::string(cur().text);
                    advance();
                }
                g.bounds.push_back(std::move(bound));
            } while (accept(TokenKind::amp));
        }
        out.push_back(std::move(g));
    } while (accept(TokenKind::comma));
    expect_close_angle();
    return out;
}

std::vector<Param> Parser::parse_params() {
    std::vector<Param> out;
    skip_newlines();
    if (check(TokenKind::rparen)) return out;

    if (check(TokenKind::kw_self)) {
        error_at(cur(), "self is implicit in instance methods — remove it from the parameter list");
        advance();
        if (!accept(TokenKind::comma)) return out;
    }

    do {
        skip_newlines();
        Param p;
        if (accept(TokenKind::kw_move)) p.passing = Param::Passing::move;
        else if (check(TokenKind::kw_take)) {
            error_at(cur(), "'take' was removed — use 'move'");
            advance();
            p.passing = Param::Passing::move;
        }
        else if (accept(TokenKind::kw_inout)) p.passing = Param::Passing::inout;
        if (check(TokenKind::ident)) {
            p.line = cur().line;
            p.col = cur().col;
            p.name = std::string(cur().text);
            advance();
        } else {
            error_at(cur(), "expected parameter name");
            break;
        }
        expect(TokenKind::colon, "':' after parameter name");
        p.type = parse_type();
        out.push_back(std::move(p));
        skip_newlines();
    } while (accept(TokenKind::comma));
    return out;
}

// ---- types ----------------------------------------------------------------

TypePtr Parser::parse_type() {
    auto t = std::make_unique<TypeRef>();
    t->line = cur().line;
    t->col = cur().col;

    if (accept(TokenKind::lbracket)) {
        t->kind = TypeRef::Kind::fixed_array;
        t->array_elem = parse_type();
        expect(TokenKind::semicolon, "';' in fixed array type");
        if (!check(TokenKind::int_lit)) {
            error_at(cur(), "expected fixed array length");
        } else {
            std::string digits;
            for (char c : cur().text) if (c != '_') digits.push_back(c);
            unsigned long long length = std::strtoull(digits.c_str(), nullptr, 10);
            if (length > UINT32_MAX) {
                error_at(cur(), "fixed array length is too large");
            } else {
                t->array_len = static_cast<uint32_t>(length);
            }
            advance();
        }
        expect(TokenKind::rbracket, "']' in fixed array type");
        stamp_end(*t);
        return t;
    }

    if (accept(TokenKind::kw_fn)) {
        t->kind = TypeRef::Kind::fn;
        expect(TokenKind::lparen, "'(' in fn type");
        if (!check(TokenKind::rparen)) {
            do {
                t->fn_params.push_back(parse_type());
            } while (accept(TokenKind::comma));
        }
        expect(TokenKind::rparen, "')'");
        if (accept(TokenKind::arrow)) t->fn_ret = parse_type();
        stamp_end(*t);
        return t;
    }

    if (check(TokenKind::ident)) {
        t->name = std::string(cur().text);
        advance();
        // package-qualified type: `util.User`
        if (check(TokenKind::dot) && next().kind == TokenKind::ident) {
            advance();
            t->name += '.';
            t->name += std::string(cur().text);
            advance();
        }
    } else {
        error_at(cur(), "expected type");
        return t;
    }
    if (accept(TokenKind::lt)) {
        do {
            skip_newlines();
            t->args.push_back(parse_type());
            skip_newlines();
        } while (accept(TokenKind::comma));
        expect_close_angle();
    }
    stamp_end(*t);
    return t;
}

// ---- statements -----------------------------------------------------------

std::vector<StmtPtr> Parser::parse_block() {
    std::vector<StmtPtr> out;
    expect(TokenKind::lbrace, "'{'");
    skip_newlines();
    while (!check(TokenKind::rbrace) && !at_eof()) {
        size_t before = errors_.size();
        StmtPtr s = parse_stmt();
        if (s) { stamp_end(*s); out.push_back(std::move(s)); }
        if (errors_.size() > before) sync_stmt();
        skip_newlines();
    }
    expect(TokenKind::rbrace, "'}'");
    return out;
}

StmtPtr Parser::parse_stmt() {
    switch (cur().kind) {
        case TokenKind::kw_let: { advance(); return parse_let(false); }
        case TokenKind::kw_var: { advance(); return parse_let(true); }
        case TokenKind::kw_return: {
            auto s = new_stmt(Stmt::Kind::ret, cur());
            advance();
            if (!check(TokenKind::newline) && !check(TokenKind::rbrace) && !at_eof()) {
                s->expr = parse_expr();
            }
            end_stmt();
            return s;
        }
        case TokenKind::kw_break: {
            auto s = new_stmt(Stmt::Kind::brk, cur());
            advance();
            end_stmt();
            return s;
        }
        case TokenKind::kw_continue: {
            auto s = new_stmt(Stmt::Kind::cont, cur());
            advance();
            end_stmt();
            return s;
        }
        case TokenKind::kw_defer: {
            auto s = new_stmt(Stmt::Kind::defer_, cur());
            advance();
            s->expr = parse_expr();
            end_stmt();
            return s;
        }
        case TokenKind::kw_unsafe: {
            auto s = new_stmt(Stmt::Kind::unsafe_, cur());
            advance();
            s->body = parse_block();
            end_stmt();
            return s;
        }
        case TokenKind::kw_if:
            return parse_if_stmt();
        case TokenKind::kw_for:
            return parse_for();
        default: {
            auto s = new_stmt(Stmt::Kind::expr, cur());
            s->expr = parse_expr();
            if (is_assign_op(cur().kind)) {
                s->kind = Stmt::Kind::assign;
                s->op = cur().kind;
                advance();
                s->target = std::move(s->expr);
                s->value = parse_expr();
            }
            end_stmt();
            return s;
        }
    }
}

StmtPtr Parser::parse_let(bool is_var) {
    auto s = new_stmt(Stmt::Kind::let_, cur());
    s->is_var = is_var;
    if (check(TokenKind::ident)) {
        s->name = std::string(cur().text);
        advance();
    } else {
        error_at(cur(), "expected variable name");
    }
    expect(TokenKind::colon, "':' — beans requires the type here");
    s->type = parse_type();
    if (accept(TokenKind::assign)) s->init = parse_expr();
    end_stmt();
    return s;
}

StmtPtr Parser::parse_if_stmt() {
    auto s = new_stmt(Stmt::Kind::if_, cur());
    advance(); // if
    {
        StructGuard g(*this, false);
        s->cond = parse_expr();
    }
    s->body = parse_block();
    if (accept(TokenKind::kw_else)) {
        if (check(TokenKind::kw_if)) {
            s->else_body.push_back(parse_if_stmt());
        } else {
            s->else_body = parse_block();
        }
    }
    stamp_end(*s);
    return s;
}

StmtPtr Parser::parse_for() {
    auto s = new_stmt(Stmt::Kind::for_ever, cur());
    advance(); // for

    if (check(TokenKind::lbrace)) {
        s->body = parse_block();
        return s;
    }

    // `for name: Type in iterable`
    if (check(TokenKind::ident) && next().kind == TokenKind::colon) {
        s->kind = Stmt::Kind::for_in;
        s->loop_var = std::string(cur().text);
        advance();
        advance(); // :
        s->loop_type = parse_type();
        expect(TokenKind::kw_in, "'in'");
        {
            StructGuard g(*this, false);
            s->iterable = parse_expr();
        }
        s->body = parse_block();
        return s;
    }

    s->kind = Stmt::Kind::for_while;
    {
        StructGuard g(*this, false);
        s->cond = parse_expr();
    }
    s->body = parse_block();
    return s;
}

// ---- expressions ----------------------------------------------------------

ExprPtr Parser::parse_expr() {
    ExprPtr lhs = parse_or();
    if (check(TokenKind::dotdot) || check(TokenKind::dotdoteq)) {
        auto e = new_expr(Expr::Kind::range, cur());
        e->inclusive = check(TokenKind::dotdoteq);
        advance();
        e->lhs = std::move(lhs);
        e->rhs = parse_or();
        stamp_end(*e);
        return e;
    }
    return lhs;
}

// one helper per precedence level keeps this dead simple
#define BINARY_LEVEL(name, sub, COND)                                  \
    ExprPtr Parser::name() {                                           \
        ExprPtr lhs = sub();                                           \
        while (COND) {                                                 \
            auto e = new_expr(Expr::Kind::binary, cur());              \
            e->op = cur().kind;                                        \
            advance();                                                 \
            e->lhs = std::move(lhs);                                   \
            e->rhs = sub();                                            \
            stamp_end(*e);                                             \
            lhs = std::move(e);                                        \
        }                                                              \
        return lhs;                                                    \
    }

BINARY_LEVEL(parse_or, parse_and, check(TokenKind::oror))
BINARY_LEVEL(parse_and, parse_equality, check(TokenKind::andand))
BINARY_LEVEL(parse_equality, parse_comparison,
             check(TokenKind::eq) || check(TokenKind::neq))
BINARY_LEVEL(parse_comparison, parse_bitor,
             check(TokenKind::lt) || check(TokenKind::le) ||
             check(TokenKind::gt) || check(TokenKind::ge))
BINARY_LEVEL(parse_bitor, parse_bitxor, check(TokenKind::pipe))
BINARY_LEVEL(parse_bitxor, parse_bitand, check(TokenKind::caret))
BINARY_LEVEL(parse_bitand, parse_shift, check(TokenKind::amp))
BINARY_LEVEL(parse_shift, parse_additive,
             check(TokenKind::shl) || check(TokenKind::shr))
BINARY_LEVEL(parse_additive, parse_multiplicative,
             check(TokenKind::plus) || check(TokenKind::minus))
BINARY_LEVEL(parse_multiplicative, parse_cast,
             check(TokenKind::star) || check(TokenKind::slash) ||
             check(TokenKind::percent))

#undef BINARY_LEVEL

ExprPtr Parser::parse_cast() {
    ExprPtr e = parse_unary();
    while (check(TokenKind::kw_as)) {
        auto c = new_expr(Expr::Kind::cast, cur());
        advance();
        c->checked = accept(TokenKind::question);
        c->object = std::move(e);
        c->type = parse_type();
        stamp_end(*c);
        e = std::move(c);
    }
    return e;
}

ExprPtr Parser::parse_unary() {
    if (check(TokenKind::minus) || check(TokenKind::bang) || check(TokenKind::tilde) ||
        check(TokenKind::kw_move) || check(TokenKind::kw_take) ||
        check(TokenKind::kw_inout)) {
        auto e = new_expr(Expr::Kind::unary, cur());
        if (check(TokenKind::kw_take)) {
            error_at(cur(), "'take' was removed — use 'move'");
            e->op = TokenKind::kw_move;
        } else {
            e->op = cur().kind;
        }
        advance();
        e->rhs = parse_unary();
        stamp_end(*e);
        return e;
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    ExprPtr e = parse_primary();
    stamp_end(*e);
    while (true) {
        if (check(TokenKind::dot)) {
            auto f = new_expr(Expr::Kind::field, cur());
            advance();
            if (check(TokenKind::ident)) {
                f->name = std::string(cur().text);
                advance();
            } else if (check(TokenKind::kw_new)) {
                error_at(cur(), "'.new(...)' was removed — use 'new Type(...)'");
                f->name = "new";
                advance();
            } else {
                error_at(cur(), "expected name after '.'");
            }
            stamp_end(*f); // ends just past the member name
            f->object = std::move(e);
            e = std::move(f);
        } else if (check(TokenKind::lparen)) {
            auto c = new_expr(Expr::Kind::call, cur());
            advance();
            c->args = parse_call_args();
            stamp_end(*c);
            c->callee = std::move(e);
            e = std::move(c);
        } else if (check(TokenKind::lbracket)) {
            auto i = new_expr(Expr::Kind::index, cur());
            advance();
            i->index_expr = parse_expr();
            expect(TokenKind::rbracket, "']'");
            stamp_end(*i);
            i->object = std::move(e);
            e = std::move(i);
        } else if (check(TokenKind::question)) {
            auto t = new_expr(Expr::Kind::try_, cur());
            advance();
            stamp_end(*t);
            t->object = std::move(e);
            e = std::move(t);
        } else {
            return e;
        }
    }
}

std::vector<ExprPtr> Parser::parse_call_args() {
    // `(` already consumed; struct inits are fine again inside parens
    StructGuard g(*this, true);
    std::vector<ExprPtr> args;
    skip_newlines();
    if (accept(TokenKind::rparen)) return args;
    do {
        skip_newlines();
        args.push_back(parse_expr());
        skip_newlines();
    } while (accept(TokenKind::comma) && !check(TokenKind::rparen));
    skip_newlines();
    expect(TokenKind::rparen, "')'");
    return args;
}

ExprPtr Parser::parse_primary() {
    const Token& t = cur();
    switch (t.kind) {
        case TokenKind::int_lit: {
            auto e = new_expr(Expr::Kind::int_lit, t);
            e->text = t.text;
            advance();
            return e;
        }
        case TokenKind::float_lit: {
            auto e = new_expr(Expr::Kind::float_lit, t);
            e->text = t.text;
            advance();
            return e;
        }
        case TokenKind::string_lit: {
            auto e = new_expr(Expr::Kind::string_lit, t);
            e->text = t.text;
            advance();
            return e;
        }
        case TokenKind::kw_true:
        case TokenKind::kw_false: {
            auto e = new_expr(Expr::Kind::bool_lit, t);
            e->bool_val = t.kind == TokenKind::kw_true;
            advance();
            return e;
        }
        case TokenKind::kw_self: {
            auto e = new_expr(Expr::Kind::self_ref, t);
            advance();
            return e;
        }
        case TokenKind::kw_fn:
            return parse_closure();
        case TokenKind::kw_if:
            return parse_if_expr();
        case TokenKind::kw_match:
            return parse_match_expr();
        case TokenKind::kw_new: {
            auto e = new_expr(Expr::Kind::new_, t);
            advance();
            TypePtr type = parse_type();
            if (type->kind != TypeRef::Kind::named || type->name.empty()) {
                error_at(t, "new needs a class name");
            } else {
                e->name = std::move(type->name);
                e->type_args = std::move(type->args);
            }
            expect(TokenKind::lparen, "'(' after class name");
            e->args = parse_call_args();
            return e;
        }
        case TokenKind::lparen: {
            advance();
            StructGuard g(*this, true);
            ExprPtr e = parse_expr();
            expect(TokenKind::rparen, "')'");
            return e;
        }
        case TokenKind::lbracket: {
            auto e = new_expr(Expr::Kind::list_lit, t);
            advance();
            StructGuard g(*this, true);
            skip_newlines();
            if (!check(TokenKind::rbracket)) {
                do {
                    skip_newlines();
                    e->args.push_back(parse_expr());
                    skip_newlines();
                } while (accept(TokenKind::comma) && !check(TokenKind::rbracket));
            }
            skip_newlines();
            expect(TokenKind::rbracket, "']'");
            return e;
        }
        case TokenKind::lbrace: {
            // short init / map literal: `{ ... }` — target type comes from context
            if (!allow_struct_) {
                error_at(t, "unexpected '{'");
                advance();
                return new_expr(Expr::Kind::init, t);
            }
            advance();
            return parse_init_body("", {}, t);
        }
        case TokenKind::ident: {
            auto e = new_expr(Expr::Kind::ident, t);
            e->text = t.text;
            advance();

            if (allow_struct_) {
                // `Name {` / `pkg.Name {` / `Name<T> {` / `pkg.Name<T> {`
                // initializers. Speculative: rewind fully if no `{` commits,
                // so `pkg.field` stays a field access and `<` a comparison.
                size_t save_pos = pos_;
                size_t save_errs = errors_.size();
                std::string init_name(t.text);
                if (check(TokenKind::dot) && next().kind == TokenKind::ident) {
                    advance();
                    init_name += '.';
                    init_name += std::string(cur().text);
                    advance();
                }
                if (check(TokenKind::lbrace)) {
                    advance();
                    return parse_init_body(std::move(init_name), {}, t);
                }
                if (check(TokenKind::lt)) {
                    advance(); // <
                    std::vector<TypePtr> targs;
                    bool ok = true;
                    do {
                        if (!check(TokenKind::ident) && !check(TokenKind::kw_fn)) {
                            ok = false;
                            break;
                        }
                        targs.push_back(parse_type());
                    } while (accept(TokenKind::comma));
                    if (ok && errors_.size() == save_errs &&
                        (check(TokenKind::gt) || check(TokenKind::shr))) {
                        expect_close_angle();
                        if (check(TokenKind::lbrace) && errors_.size() == save_errs) {
                            advance();
                            return parse_init_body(std::move(init_name),
                                                   std::move(targs), t);
                        }
                    }
                }
                // not an initializer
                pos_ = save_pos;
                errors_.resize(save_errs);
            }
            return e;
        }
        default:
            error_at(t, "expected expression");
            advance();
            return new_expr(Expr::Kind::ident, t);
    }
}

ExprPtr Parser::parse_init_body(std::string type_name,
                                std::vector<TypePtr> type_args,
                                const Token& start) {
    // `{` already consumed
    auto e = new_expr(Expr::Kind::init, start);
    e->name = std::move(type_name);
    e->type_args = std::move(type_args);

    StructGuard g(*this, true);
    skip_newlines();
    while (!check(TokenKind::rbrace) && !at_eof()) {
        InitEntry entry;
        if (check(TokenKind::ident) && next().kind == TokenKind::colon) {
            entry.name = std::string(cur().text);
            advance();
            advance(); // :
        } else {
            entry.key = parse_expr();
            expect(TokenKind::colon, "':' between key and value");
        }
        entry.value = parse_expr();
        e->entries.push_back(std::move(entry));
        skip_newlines();
        if (!accept(TokenKind::comma)) break;
        skip_newlines();
    }
    skip_newlines();
    expect(TokenKind::rbrace, "'}'");
    return e;
}

ExprPtr Parser::parse_closure() {
    auto e = new_expr(Expr::Kind::closure, cur());
    advance(); // fn
    expect(TokenKind::lparen, "'('");
    e->params = parse_params();
    expect(TokenKind::rparen, "')'");
    if (accept(TokenKind::arrow)) e->type = parse_type();
    e->body = parse_block();
    return e;
}

ExprPtr Parser::parse_if_expr() {
    auto e = new_expr(Expr::Kind::if_expr, cur());
    advance(); // if
    {
        StructGuard g(*this, false);
        e->cond = parse_expr();
    }
    expect(TokenKind::lbrace, "'{'");
    {
        StructGuard g(*this, true);
        skip_newlines();
        e->then_e = parse_expr();
        skip_newlines();
    }
    expect(TokenKind::rbrace, "'}'");
    expect(TokenKind::kw_else, "'else' — if used as a value needs both sides");
    if (check(TokenKind::kw_if)) {
        e->else_e = parse_if_expr();
    } else {
        expect(TokenKind::lbrace, "'{'");
        StructGuard g(*this, true);
        skip_newlines();
        e->else_e = parse_expr();
        skip_newlines();
        expect(TokenKind::rbrace, "'}'");
    }
    return e;
}

ExprPtr Parser::parse_match_expr() {
    auto e = new_expr(Expr::Kind::match_expr, cur());
    advance(); // match
    {
        StructGuard g(*this, false);
        e->subject = parse_expr();
    }
    expect(TokenKind::lbrace, "'{'");
    skip_newlines();
    while (!check(TokenKind::rbrace) && !at_eof()) {
        MatchArm arm;
        arm.pat = parse_pattern();
        expect(TokenKind::fat_arrow, "'=>'");
        if (check(TokenKind::lbrace)) {
            // block arm: `pattern => { stmts }` — statement matches only; the
            // checker rejects it where the match must produce a value. A map
            // literal as an arm value needs parens: `x => ({"a": 1})`.
            arm.is_block = true;
            arm.body = parse_block();
            e->arms.push_back(std::move(arm));
            skip_newlines();
            accept(TokenKind::comma); // optional after a block
            skip_newlines();
            continue;
        }
        {
            StructGuard g(*this, true);
            skip_newlines();
            arm.value = parse_expr();
        }
        e->arms.push_back(std::move(arm));
        skip_newlines();
        if (!accept(TokenKind::comma)) break;
        skip_newlines();
    }
    skip_newlines();
    expect(TokenKind::rbrace, "'}'");
    return e;
}

// ---- patterns -------------------------------------------------------------

PatPtr Parser::parse_pattern() {
    PatPtr first = parse_pattern_atom();
    if (!check(TokenKind::pipe)) return first;

    auto alt = std::make_unique<Pattern>();
    alt->kind = Pattern::Kind::alt;
    alt->line = first->line;
    alt->col = first->col;
    alt->alts.push_back(std::move(first));
    while (accept(TokenKind::pipe)) {
        alt->alts.push_back(parse_pattern_atom());
    }
    return alt;
}

PatPtr Parser::parse_pattern_atom() {
    auto p = std::make_unique<Pattern>();
    p->line = cur().line;
    p->col = cur().col;

    // literal (possibly negative, possibly a range)
    if (check(TokenKind::int_lit) || check(TokenKind::float_lit) ||
        check(TokenKind::string_lit) || check(TokenKind::kw_true) ||
        check(TokenKind::kw_false) || check(TokenKind::minus)) {
        p->kind = Pattern::Kind::literal;
        if (check(TokenKind::minus)) {
            auto neg = new_expr(Expr::Kind::unary, cur());
            neg->op = TokenKind::minus;
            advance();
            neg->rhs = parse_primary();
            p->lit = std::move(neg);
        } else {
            p->lit = parse_primary();
        }
        if (check(TokenKind::dotdot) || check(TokenKind::dotdoteq)) {
            p->kind = Pattern::Kind::range;
            p->inclusive = check(TokenKind::dotdoteq);
            advance();
            p->lit2 = parse_primary();
        }
        return p;
    }

    if (check(TokenKind::ident)) {
        if (cur().text == "_") {
            p->kind = Pattern::Kind::wildcard;
            advance();
            return p;
        }
        p->kind = Pattern::Kind::name;
        p->name = std::string(cur().text);
        advance();
        if (accept(TokenKind::lparen)) {
            p->has_payload = true;
            skip_newlines();
            if (!check(TokenKind::rparen)) {
                do {
                    skip_newlines();
                    Param b;
                    if (check(TokenKind::ident)) {
                        b.line = cur().line;
                        b.col = cur().col;
                        b.name = std::string(cur().text);
                        advance();
                    } else {
                        error_at(cur(), "expected binding name");
                        break;
                    }
                    if (accept(TokenKind::colon)) b.type = parse_type();
                    p->bindings.push_back(std::move(b));
                    skip_newlines();
                } while (accept(TokenKind::comma));
            }
            expect(TokenKind::rparen, "')'");
        }
        return p;
    }

    error_at(cur(), "expected pattern");
    p->kind = Pattern::Kind::wildcard;
    advance();
    return p;
}

} // namespace beans
