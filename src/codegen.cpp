#include "codegen.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

#include "lexer.h"
#include "parser.h"

namespace beans {

namespace {

// value categories the native backend knows
enum class T { i64, f64, i1, str, unit, bad };

T type_of_ref(const TypeRef* t) {
    if (!t) return T::unit;
    if (t->kind != TypeRef::Kind::named) return T::bad;
    const std::string& n = t->name;
    if (n == "int" || n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
        n == "u8" || n == "u16" || n == "u32" || n == "u64" || n == "byte")
        return T::i64;
    if (n == "float" || n == "f32" || n == "f64") return T::f64;
    if (n == "bool") return T::i1;
    if (n == "string") return T::str;
    return T::bad;
}

const char* ll(T t) {
    switch (t) {
        case T::i64: return "i64";
        case T::f64: return "double";
        case T::i1: return "i1";
        case T::str: return "ptr";
        case T::unit: return "void";
        case T::bad: return "void";
    }
    return "void";
}

std::string fmt_double(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(bits));
    return buf;
}

int64_t parse_int_text(std::string_view text) {
    std::string clean;
    for (char c : text) {
        if (c != '_') clean.push_back(c);
    }
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X'))
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 16));
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B'))
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 2));
    return static_cast<int64_t>(std::strtoll(clean.c_str(), nullptr, 10));
}

double parse_float_text(std::string_view text) {
    std::string clean;
    for (char c : text) {
        if (c != '_') clean.push_back(c);
    }
    return std::strtod(clean.c_str(), nullptr);
}

// split "a {x} b" into literal pieces and parsed expression pieces.
// segment sources are owned by `srcs` so the ASTs' string_views stay valid.
struct StrPiece {
    std::string text;
    ExprPtr expr;
};
std::vector<StrPiece> split_interp(std::string_view raw,
                                   std::vector<std::unique_ptr<std::string>>& srcs) {
    std::vector<StrPiece> parts;
    std::string_view body =
        raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : std::string_view{};
    std::string cur;
    size_t i = 0;
    while (i < body.size()) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            char n = body[i + 1];
            switch (n) {
                case 'n': cur += '\n'; break;
                case 't': cur += '\t'; break;
                case 'r': cur += '\r'; break;
                case '0': cur += '\0'; break;
                case '\\': cur += '\\'; break;
                case '"': cur += '"'; break;
                case '{': cur += '{'; break;
                case '}': cur += '}'; break;
                default: cur += n; break;
            }
            i += 2;
            continue;
        }
        if (c == '{') {
            size_t start = i + 1;
            int depth = 1;
            size_t j = start;
            bool in_str = false;
            while (j < body.size() && depth > 0) {
                char d = body[j];
                if (d == '\\') { j += 2; continue; }
                if (in_str) {
                    if (d == '"') in_str = false;
                } else if (d == '"') {
                    in_str = true;
                } else if (d == '{') {
                    depth += 1;
                } else if (d == '}') {
                    depth -= 1;
                }
                j += 1;
            }
            if (!cur.empty()) {
                parts.push_back({cur, nullptr});
                cur.clear();
            }
            srcs.push_back(std::make_unique<std::string>(body.substr(start, j - 1 - start)));
            Lexer lx(*srcs.back());
            Parser ps(lx.scan_all());
            parts.push_back({"", ps.parse_standalone_expr()});
            i = j;
            continue;
        }
        cur += c;
        i += 1;
    }
    if (!cur.empty()) parts.push_back({cur, nullptr});
    return parts;
}

} // namespace

// ---- module-level ----------------------------------------------------------

CodeGen::CodeGen(const Module& mod) : mod_(mod) {}

void CodeGen::error_at(uint32_t line, uint32_t col, std::string msg) {
    errors_.push_back({std::move(msg), line, col});
}

std::string CodeGen::intern_string(const std::string& bytes) {
    std::string name = "@.str" + std::to_string(next_str_++);
    globals_ += name + " = private unnamed_addr constant [" +
                std::to_string(bytes.size() + 1) + " x i8] c\"";
    for (unsigned char c : bytes) {
        if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') {
            globals_ += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "\\%02X", c);
            globals_ += buf;
        }
    }
    globals_ += "\\00\"\n";
    return name;
}

// ---- per-function emitter ---------------------------------------------------

struct CodeGen::Fn {
    CodeGen& cg;
    const FnDecl& decl;
    bool is_main;

    std::string allocas;   // emitted into the entry block
    std::string body;      // everything after
    int next_reg = 0;
    int next_bb = 0;
    bool terminated = false;

    struct Var { std::string slot; T type; };
    std::vector<std::map<std::string, Var>> scopes;
    std::vector<std::pair<std::string, std::string>> loop_stack; // (break, continue)
    std::vector<std::unique_ptr<std::string>> interp_srcs;

    Fn(CodeGen& cg, const FnDecl& d, bool m) : cg(cg), decl(d), is_main(m) {}

    std::string reg() { return "%t" + std::to_string(next_reg++); }
    std::string bb() { return "bb" + std::to_string(next_bb++); }
    void line(const std::string& s) { body += "  " + s + "\n"; }
    void label(const std::string& l) {
        body += l + ":\n";
        terminated = false;
    }
    void err(const Expr* e, const std::string& msg) {
        cg.error_at(e ? e->line : decl.line, e ? e->col : decl.col,
                    msg + " — not in the native backend yet (beansc run still works)");
    }
    void err_stmt(const Stmt* s, const std::string& msg) {
        cg.error_at(s->line, s->col,
                    msg + " — not in the native backend yet (beansc run still works)");
    }

    Var* find_var(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    std::string alloc_slot(const std::string& name, T t) {
        std::string slot = "%v" + std::to_string(next_reg++) + "." + name;
        allocas += "  " + slot + " = alloca " + ll(t) + "\n";
        scopes.back()[name] = {slot, t};
        return slot;
    }

    const FnDecl* find_fn(const std::string& name) {
        for (const FnDecl& f : cg.mod_.fns) {
            if (f.name == name) return &f;
        }
        return nullptr;
    }

    // ---- expressions: returns (value text, type) ----
    // (Named eval per compiler convention, but this executes nothing —
    // it only EMITS LLVM IR text for the expression. No eval() risk applies.)
    std::pair<std::string, T> eval(const Expr* e) {
        switch (e->kind) {
            case Expr::Kind::int_lit:
                return {std::to_string(parse_int_text(e->text)), T::i64};
            case Expr::Kind::float_lit:
                return {fmt_double(parse_float_text(e->text)), T::f64};
            case Expr::Kind::bool_lit:
                return {e->bool_val ? "1" : "0", T::i1};
            case Expr::Kind::string_lit:
                return eval_string(e);
            case Expr::Kind::ident: {
                std::string name(e->text);
                if (Var* v = find_var(name)) {
                    std::string r = reg();
                    line(r + " = load " + ll(v->type) + ", ptr " + v->slot);
                    return {r, v->type};
                }
                err(e, "reading '" + name + "' here");
                return {"0", T::i64};
            }
            case Expr::Kind::unary: {
                auto [v, t] = eval(e->rhs.get());
                std::string r = reg();
                if (e->op == TokenKind::minus) {
                    if (t == T::f64) line(r + " = fneg double " + v);
                    else line(r + " = sub i64 0, " + v);
                    return {r, t};
                }
                if (e->op == TokenKind::bang) {
                    line(r + " = xor i1 " + v + ", 1");
                    return {r, T::i1};
                }
                line(r + " = xor i64 " + v + ", -1");
                return {r, T::i64};
            }
            case Expr::Kind::binary:
                return eval_binary(e);
            case Expr::Kind::call:
                return eval_call(e);
            case Expr::Kind::cast: {
                auto [v, t] = eval(e->object.get());
                T to = type_of_ref(e->type.get());
                if (t == to) return {v, t};
                std::string r = reg();
                if (t == T::i64 && to == T::f64) {
                    line(r + " = sitofp i64 " + v + " to double");
                    return {r, T::f64};
                }
                if (t == T::f64 && to == T::i64) {
                    line(r + " = fptosi double " + v + " to i64");
                    return {r, T::i64};
                }
                err(e, "this cast");
                return {v, t};
            }
            case Expr::Kind::if_expr: {
                auto [c, ct] = eval(e->cond.get());
                (void)ct;
                std::string then_bb = bb(), else_bb = bb(), end_bb = bb();
                // result slot: type discovered from the first branch
                std::string slot = "%v" + std::to_string(next_reg++) + ".ifv";
                line("br i1 " + c + ", label %" + then_bb + ", label %" + else_bb);
                label(then_bb);
                auto [a, at] = eval(e->then_e.get());
                allocas += "  " + slot + " = alloca " + ll(at) + "\n";
                line("store " + std::string(ll(at)) + " " + a + ", ptr " + slot);
                line("br label %" + end_bb);
                label(else_bb);
                auto [b2, bt] = eval(e->else_e.get());
                (void)bt;
                line("store " + std::string(ll(at)) + " " + b2 + ", ptr " + slot);
                line("br label %" + end_bb);
                label(end_bb);
                std::string r = reg();
                line(r + " = load " + std::string(ll(at)) + ", ptr " + slot);
                return {r, at};
            }
            default:
                err(e, "this expression");
                return {"0", T::i64};
        }
    }

    std::pair<std::string, T> eval_string(const Expr* e) {
        std::vector<StrPiece> parts = split_interp(e->text, interp_srcs);
        if (parts.empty()) {
            std::string g = cg.intern_string("");
            return {g, T::str};
        }
        std::string acc;
        bool have = false;
        for (StrPiece& p : parts) {
            std::string piece;
            if (p.expr) {
                auto [v, t] = eval(p.expr.get());
                piece = to_str(v, t, p.expr.get());
            } else {
                piece = cg.intern_string(p.text);
            }
            if (!have) {
                acc = piece;
                have = true;
            } else {
                std::string r = reg();
                line(r + " = call ptr @beans_concat(ptr " + acc + ", ptr " + piece + ")");
                acc = r;
            }
        }
        return {acc, T::str};
    }

    std::string to_str(const std::string& v, T t, const Expr* at) {
        std::string r = reg();
        switch (t) {
            case T::str: return v;
            case T::i64: line(r + " = call ptr @beans_from_int(i64 " + v + ")"); return r;
            case T::f64: line(r + " = call ptr @beans_from_float(double " + v + ")"); return r;
            case T::i1: {
                std::string z = reg();
                line(z + " = zext i1 " + v + " to i32");
                line(r + " = call ptr @beans_from_bool(i32 " + z + ")");
                return r;
            }
            default:
                err(at, "printing this value");
                return v;
        }
    }

    std::pair<std::string, T> eval_binary(const Expr* e) {
        // short-circuit
        if (e->op == TokenKind::andand || e->op == TokenKind::oror) {
            bool is_and = e->op == TokenKind::andand;
            std::string slot = "%v" + std::to_string(next_reg++) + ".sc";
            allocas += "  " + slot + " = alloca i1\n";
            auto [l, lt] = eval(e->lhs.get());
            (void)lt;
            line("store i1 " + l + ", ptr " + slot);
            std::string more = bb(), end = bb();
            if (is_and) line("br i1 " + l + ", label %" + more + ", label %" + end);
            else line("br i1 " + l + ", label %" + end + ", label %" + more);
            label(more);
            auto [r2, rt] = eval(e->rhs.get());
            (void)rt;
            line("store i1 " + r2 + ", ptr " + slot);
            line("br label %" + end);
            label(end);
            std::string r = reg();
            line(r + " = load i1, ptr " + slot);
            return {r, T::i1};
        }

        auto [l, lt] = eval(e->lhs.get());
        auto [r2, rt] = eval(e->rhs.get());
        (void)rt;
        std::string r = reg();

        if (lt == T::str) {
            // string compare via runtime
            std::string c = reg();
            line(c + " = call i32 @beans_str_cmp(ptr " + l + ", ptr " + r2 + ")");
            const char* pred = nullptr;
            switch (e->op) {
                case TokenKind::eq: pred = "eq"; break;
                case TokenKind::neq: pred = "ne"; break;
                case TokenKind::lt: pred = "slt"; break;
                case TokenKind::le: pred = "sle"; break;
                case TokenKind::gt: pred = "sgt"; break;
                case TokenKind::ge: pred = "sge"; break;
                default:
                    err(e, "this string operation");
                    return {"0", T::i1};
            }
            line(r + " = icmp " + pred + " i32 " + c + ", 0");
            return {r, T::i1};
        }

        bool flt = lt == T::f64;
        auto arith = [&](const char* iop, const char* fop) -> std::pair<std::string, T> {
            line(r + " = " + (flt ? fop : iop) + " " + (flt ? "double" : "i64") + " " +
                 l + ", " + r2);
            return {r, flt ? T::f64 : T::i64};
        };
        auto compare = [&](const char* ipred, const char* fpred) -> std::pair<std::string, T> {
            if (flt) line(r + " = fcmp " + std::string(fpred) + " double " + l + ", " + r2);
            else if (lt == T::i1) line(r + " = icmp " + std::string(ipred) + " i1 " + l + ", " + r2);
            else line(r + " = icmp " + std::string(ipred) + " i64 " + l + ", " + r2);
            return {r, T::i1};
        };

        switch (e->op) {
            case TokenKind::plus: return arith("add", "fadd");
            case TokenKind::minus: return arith("sub", "fsub");
            case TokenKind::star: return arith("mul", "fmul");
            case TokenKind::slash: return arith("sdiv", "fdiv");
            case TokenKind::percent: return arith("srem", "frem");
            case TokenKind::shl: return arith("shl", "shl");
            case TokenKind::shr: return arith("ashr", "ashr");
            case TokenKind::amp: return arith("and", "and");
            case TokenKind::pipe: return arith("or", "or");
            case TokenKind::caret: return arith("xor", "xor");
            case TokenKind::eq: return compare("eq", "oeq");
            case TokenKind::neq: return compare("ne", "one");
            case TokenKind::lt: return compare("slt", "olt");
            case TokenKind::le: return compare("sle", "ole");
            case TokenKind::gt: return compare("sgt", "ogt");
            case TokenKind::ge: return compare("sge", "oge");
            default:
                err(e, "this operator");
                return {"0", T::i64};
        }
    }

    std::pair<std::string, T> eval_call(const Expr* e) {
        const Expr* callee = e->callee.get();

        // io.println / io.print
        if (callee->kind == Expr::Kind::field &&
            callee->object->kind == Expr::Kind::ident) {
            std::string pkg(callee->object->text);
            if (pkg == "io" && !find_var(pkg) &&
                (callee->name == "println" || callee->name == "print")) {
                auto [v, t] = eval(e->args[0].get());
                std::string s = to_str(v, t, e->args[0].get());
                line("call void @beans_" + callee->name + "(ptr " + s + ")");
                return {"", T::unit};
            }
            err(e, "calling '" + std::string(callee->object->text) + "." + callee->name + "'");
            return {"0", T::i64};
        }

        if (callee->kind == Expr::Kind::ident) {
            std::string name(callee->text);
            const FnDecl* f = find_fn(name);
            if (!f) {
                err(e, "calling '" + name + "'");
                return {"0", T::i64};
            }
            T ret = type_of_ref(f->ret.get());
            if (ret == T::bad) {
                err(e, "this function's return type");
                ret = T::i64;
            }
            std::string args;
            for (size_t i = 0; i < e->args.size(); i++) {
                auto [v, t] = eval(e->args[i].get());
                if (i) args += ", ";
                args += std::string(ll(t)) + " " + v;
            }
            if (ret == T::unit) {
                line("call void @b_" + name + "(" + args + ")");
                return {"", T::unit};
            }
            std::string r = reg();
            line(r + " = call " + std::string(ll(ret)) + " @b_" + name + "(" + args + ")");
            return {r, ret};
        }

        err(e, "this call");
        return {"0", T::i64};
    }

    // ---- statements ----
    void exec_block(const std::vector<StmtPtr>& stmts) {
        scopes.emplace_back();
        for (const StmtPtr& s : stmts) {
            if (terminated) break; // unreachable code after return/break
            exec(s.get());
        }
        scopes.pop_back();
    }

    void exec(const Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::let_: {
                T t = type_of_ref(s->type.get());
                if (t == T::bad || t == T::unit) {
                    err_stmt(s, "a variable of this type");
                    return;
                }
                auto [v, vt] = eval(s->init.get());
                (void)vt;
                std::string slot = alloc_slot(s->name, t);
                line("store " + std::string(ll(t)) + " " + v + ", ptr " + slot);
                break;
            }
            case Stmt::Kind::assign: {
                if (s->target->kind != Expr::Kind::ident) {
                    err_stmt(s, "assigning to this");
                    return;
                }
                Var* var = find_var(std::string(s->target->text));
                if (!var) {
                    err_stmt(s, "this assignment");
                    return;
                }
                auto [v, vt] = eval(s->value.get());
                (void)vt;
                if (s->op == TokenKind::assign) {
                    line("store " + std::string(ll(var->type)) + " " + v + ", ptr " + var->slot);
                    return;
                }
                std::string cur = reg();
                line(cur + " = load " + std::string(ll(var->type)) + ", ptr " + var->slot);
                bool flt = var->type == T::f64;
                const char* op = nullptr;
                switch (s->op) {
                    case TokenKind::plus_eq: op = flt ? "fadd" : "add"; break;
                    case TokenKind::minus_eq: op = flt ? "fsub" : "sub"; break;
                    case TokenKind::star_eq: op = flt ? "fmul" : "mul"; break;
                    case TokenKind::slash_eq: op = flt ? "fdiv" : "sdiv"; break;
                    case TokenKind::percent_eq: op = flt ? "frem" : "srem"; break;
                    default: err_stmt(s, "this assignment"); return;
                }
                std::string r = reg();
                line(r + " = " + op + " " + (flt ? "double" : "i64") + " " + cur + ", " + v);
                line("store " + std::string(ll(var->type)) + " " + r + ", ptr " + var->slot);
                break;
            }
            case Stmt::Kind::expr:
                eval(s->expr.get());
                break;
            case Stmt::Kind::ret: {
                if (is_main) {
                    line("ret i32 0");
                } else if (s->expr) {
                    auto [v, t] = eval(s->expr.get());
                    line("ret " + std::string(ll(t)) + " " + v);
                } else {
                    line("ret void");
                }
                terminated = true;
                break;
            }
            case Stmt::Kind::brk:
                if (!loop_stack.empty()) {
                    line("br label %" + loop_stack.back().first);
                    terminated = true;
                }
                break;
            case Stmt::Kind::cont:
                if (!loop_stack.empty()) {
                    line("br label %" + loop_stack.back().second);
                    terminated = true;
                }
                break;
            case Stmt::Kind::if_: {
                auto [c, ct] = eval(s->cond.get());
                (void)ct;
                std::string then_bb = bb();
                std::string else_bb = s->else_body.empty() ? "" : bb();
                std::string end_bb = bb();
                line("br i1 " + c + ", label %" + then_bb + ", label %" +
                     (else_bb.empty() ? end_bb : else_bb));
                label(then_bb);
                exec_block(s->body);
                if (!terminated) line("br label %" + end_bb);
                if (!else_bb.empty()) {
                    label(else_bb);
                    exec_block(s->else_body);
                    if (!terminated) line("br label %" + end_bb);
                }
                label(end_bb);
                break;
            }
            case Stmt::Kind::for_ever: {
                std::string head = bb(), end = bb();
                line("br label %" + head);
                label(head);
                loop_stack.push_back({end, head});
                exec_block(s->body);
                loop_stack.pop_back();
                if (!terminated) line("br label %" + head);
                label(end);
                break;
            }
            case Stmt::Kind::for_while: {
                std::string head = bb(), body_bb = bb(), end = bb();
                line("br label %" + head);
                label(head);
                auto [c, ct] = eval(s->cond.get());
                (void)ct;
                line("br i1 " + c + ", label %" + body_bb + ", label %" + end);
                label(body_bb);
                loop_stack.push_back({end, head});
                exec_block(s->body);
                loop_stack.pop_back();
                if (!terminated) line("br label %" + head);
                label(end);
                break;
            }
            case Stmt::Kind::for_in: {
                if (!s->iterable || s->iterable->kind != Expr::Kind::range) {
                    err_stmt(s, "looping over this");
                    return;
                }
                auto [lo, lot] = eval(s->iterable->lhs.get());
                auto [hi, hit] = eval(s->iterable->rhs.get());
                (void)lot; (void)hit;
                scopes.emplace_back();
                std::string slot = alloc_slot(s->loop_var, T::i64);
                line("store i64 " + lo + ", ptr " + slot);
                std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
                line("br label %" + head);
                label(head);
                std::string cur = reg(), c = reg();
                line(cur + " = load i64, ptr " + slot);
                line(c + " = icmp " + (s->iterable->inclusive ? "sle" : "slt") + " i64 " +
                     cur + ", " + hi);
                line("br i1 " + c + ", label %" + body_bb + ", label %" + end);
                label(body_bb);
                loop_stack.push_back({end, step});
                exec_block(s->body);
                loop_stack.pop_back();
                if (!terminated) line("br label %" + step);
                label(step);
                std::string cur2 = reg(), nxt = reg();
                line(cur2 + " = load i64, ptr " + slot);
                line(nxt + " = add i64 " + cur2 + ", 1");
                line("store i64 " + nxt + ", ptr " + slot);
                line("br label %" + head);
                label(end);
                scopes.pop_back();
                break;
            }
            case Stmt::Kind::unsafe_:
                exec_block(s->body);
                break;
            default:
                err_stmt(s, "this statement");
                break;
        }
    }

    std::string emit() {
        T ret = is_main ? T::i64 : type_of_ref(decl.ret.get());
        if (ret == T::bad) {
            cg.error_at(decl.line, decl.col,
                        "return type of '" + decl.name +
                            "' — not in the native backend yet (beansc run still works)");
            return "";
        }

        std::string header = "define ";
        header += is_main ? "i32" : ll(ret);
        header += is_main ? " @main(" : " @b_" + decl.name + "(";
        scopes.emplace_back();

        std::string param_stores;
        for (size_t i = 0; i < decl.params.size(); i++) {
            T pt = type_of_ref(decl.params[i].type.get());
            if (pt == T::bad || pt == T::unit) {
                cg.error_at(decl.params[i].line, decl.params[i].col,
                            "parameter '" + decl.params[i].name +
                                "' — not in the native backend yet (beansc run still works)");
                pt = T::i64;
            }
            if (i) header += ", ";
            std::string preg = "%p" + std::to_string(i);
            header += std::string(ll(pt)) + " " + preg;
            std::string slot = alloc_slot(decl.params[i].name, pt);
            param_stores += "  store " + std::string(ll(pt)) + " " + preg + ", ptr " +
                            slot + "\n";
        }
        header += ") {\nentry:\n";

        std::string first = bb();
        exec_block(decl.body);
        if (!terminated) {
            if (is_main) line("ret i32 0");
            else if (ret == T::unit) line("ret void");
            else if (ret == T::f64) line("ret double " + fmt_double(0));
            else if (ret == T::str) line("ret ptr null");
            else line("ret " + std::string(ll(ret)) + " 0");
        }
        scopes.pop_back();

        return header + allocas + param_stores + "  br label %" + first + "\n" +
               first + ":\n" + body + "}\n\n";
    }
};

// ---- driver ----------------------------------------------------------------

std::string CodeGen::generate() {
    std::string fns;

    for (const auto& [kind, idx] : mod_.order) {
        if (kind != Module::DeclKind::fn) {
            const uint32_t line = kind == Module::DeclKind::class_
                                      ? mod_.classes[idx].line
                                      : mod_.enums[idx].line;
            error_at(line, 1,
                     "classes and enums are not in the native backend yet "
                     "(beansc run still works)");
            continue;
        }
        const FnDecl& f = mod_.fns[idx];
        if (!f.generics.empty()) {
            error_at(f.line, f.col,
                     "generic functions are not in the native backend yet "
                     "(beansc run still works)");
            continue;
        }
        Fn emitter(*this, f, f.name == "main");
        fns += emitter.emit();
    }

    if (!errors_.empty()) return "";

    std::string out;
    out += "; generated by beansc\n";
    out += "declare ptr @beans_from_int(i64)\n";
    out += "declare ptr @beans_from_float(double)\n";
    out += "declare ptr @beans_from_bool(i32)\n";
    out += "declare ptr @beans_concat(ptr, ptr)\n";
    out += "declare void @beans_println(ptr)\n";
    out += "declare void @beans_print(ptr)\n";
    out += "declare i32 @beans_str_cmp(ptr, ptr)\n\n";
    out += globals_;
    out += "\n";
    out += fns;
    return out;
}

const char* CodeGen::runtime_c() {
    return R"RT(// beans native runtime v1 — string helpers.
// v1 note: strings are heap-allocated and never freed (no RC yet).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* beans_from_int(long long v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", v);
    return strdup(b);
}
char* beans_from_float(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.10g", v);
    return strdup(b);
}
char* beans_from_bool(int v) { return strdup(v ? "true" : "false"); }
char* beans_concat(char* a, char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* r = malloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}
void beans_println(char* s) {
    fputs(s, stdout);
    fputc('\n', stdout);
}
void beans_print(char* s) { fputs(s, stdout); }
int beans_str_cmp(char* a, char* b) { return strcmp(a, b); }
)RT";
}

} // namespace beans
