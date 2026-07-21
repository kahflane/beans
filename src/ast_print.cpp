// human-readable AST dump — for eyeballing what the parser built

#include <string>

#include "ast.h"

namespace beans {

namespace {

std::string ind(int n) { return std::string(static_cast<size_t>(n) * 2, ' '); }

std::string type_str(const TypeRef* t) {
    if (!t) return "?";
    if (t->kind == TypeRef::Kind::fn) {
        std::string s = "fn(";
        for (size_t i = 0; i < t->fn_params.size(); i++) {
            if (i) s += ", ";
            s += type_str(t->fn_params[i].get());
        }
        s += ")";
        if (t->fn_ret) s += " -> " + type_str(t->fn_ret.get());
        return s;
    }
    std::string s = t->name;
    if (!t->args.empty()) {
        s += "<";
        for (size_t i = 0; i < t->args.size(); i++) {
            if (i) s += ", ";
            s += type_str(t->args[i].get());
        }
        s += ">";
    }
    return s;
}

std::string expr_str(const Expr* e, int depth);
void stmt_str(std::string& out, const Stmt* s, int depth);

std::string params_str(const std::vector<Param>& ps, bool self_first) {
    std::string s = "(";
    bool first = true;
    if (self_first) { s += "self"; first = false; }
    for (const Param& p : ps) {
        if (!first) s += ", ";
        first = false;
        s += p.name;
        if (p.type) s += ": " + type_str(p.type.get());
    }
    s += ")";
    return s;
}

std::string pattern_str(const Pattern* p) {
    switch (p->kind) {
        case Pattern::Kind::wildcard:
            return "_";
        case Pattern::Kind::literal:
            return expr_str(p->lit.get(), 0);
        case Pattern::Kind::range:
            return expr_str(p->lit.get(), 0) + (p->inclusive ? "..=" : "..") +
                   expr_str(p->lit2.get(), 0);
        case Pattern::Kind::name: {
            std::string s = p->name;
            if (p->has_payload) s += params_str(p->bindings, false);
            return s;
        }
        case Pattern::Kind::alt: {
            std::string s;
            for (size_t i = 0; i < p->alts.size(); i++) {
                if (i) s += " | ";
                s += pattern_str(p->alts[i].get());
            }
            return s;
        }
    }
    return "?";
}

std::string block_str(const std::vector<StmtPtr>& body, int depth) {
    std::string out = "{\n";
    for (const StmtPtr& s : body) stmt_str(out, s.get(), depth + 1);
    out += ind(depth) + "}";
    return out;
}

std::string expr_str(const Expr* e, int depth) {
    if (!e) return "<null>";
    switch (e->kind) {
        case Expr::Kind::int_lit:
        case Expr::Kind::float_lit:
        case Expr::Kind::ident:
            return std::string(e->text);
        case Expr::Kind::string_lit:
            return std::string(e->text);
        case Expr::Kind::bool_lit:
            return e->bool_val ? "true" : "false";
        case Expr::Kind::self_ref:
            return "self";
        case Expr::Kind::unary:
            return std::string("(") + to_string(e->op) + expr_str(e->rhs.get(), depth) + ")";
        case Expr::Kind::binary:
            return "(" + expr_str(e->lhs.get(), depth) + " " + to_string(e->op) +
                   " " + expr_str(e->rhs.get(), depth) + ")";
        case Expr::Kind::range:
            return "(" + expr_str(e->lhs.get(), depth) +
                   (e->inclusive ? " ..= " : " .. ") +
                   expr_str(e->rhs.get(), depth) + ")";
        case Expr::Kind::call: {
            std::string s = expr_str(e->callee.get(), depth) + "(";
            for (size_t i = 0; i < e->args.size(); i++) {
                if (i) s += ", ";
                s += expr_str(e->args[i].get(), depth);
            }
            return s + ")";
        }
        case Expr::Kind::field:
            return expr_str(e->object.get(), depth) + "." + e->name;
        case Expr::Kind::index:
            return expr_str(e->object.get(), depth) + "[" +
                   expr_str(e->index_expr.get(), depth) + "]";
        case Expr::Kind::list_lit: {
            std::string s = "[";
            for (size_t i = 0; i < e->args.size(); i++) {
                if (i) s += ", ";
                s += expr_str(e->args[i].get(), depth);
            }
            return s + "]";
        }
        case Expr::Kind::init: {
            std::string s = e->name.empty() ? "" : e->name;
            if (!e->type_args.empty()) {
                s += "<";
                for (size_t i = 0; i < e->type_args.size(); i++) {
                    if (i) s += ", ";
                    s += type_str(e->type_args[i].get());
                }
                s += ">";
            }
            if (!s.empty()) s += " ";
            s += "{";
            for (size_t i = 0; i < e->entries.size(); i++) {
                if (i) s += ",";
                s += " ";
                const InitEntry& en = e->entries[i];
                if (!en.name.empty()) s += en.name + ": ";
                else if (en.key) s += expr_str(en.key.get(), depth) + ": ";
                s += expr_str(en.value.get(), depth);
            }
            s += e->entries.empty() ? "}" : " }";
            return s;
        }
        case Expr::Kind::cast:
            return "(" + expr_str(e->object.get(), depth) +
                   (e->checked ? " as? " : " as ") + type_str(e->type.get()) + ")";
        case Expr::Kind::try_:
            return expr_str(e->object.get(), depth) + "?";
        case Expr::Kind::closure: {
            std::string s = "fn" + params_str(e->params, false);
            if (e->type) s += " -> " + type_str(e->type.get());
            s += " " + block_str(e->body, depth);
            return s;
        }
        case Expr::Kind::if_expr:
            return "if " + expr_str(e->cond.get(), depth) + " { " +
                   expr_str(e->then_e.get(), depth) + " } else " +
                   (e->else_e && e->else_e->kind == Expr::Kind::if_expr
                        ? expr_str(e->else_e.get(), depth)
                        : "{ " + expr_str(e->else_e.get(), depth) + " }");
        case Expr::Kind::match_expr: {
            std::string s = "match " + expr_str(e->subject.get(), depth) + " {\n";
            for (const MatchArm& a : e->arms) {
                s += ind(depth + 1) + pattern_str(a.pat.get()) + " => ";
                if (a.is_block) {
                    s += block_str(a.body, depth + 1);
                } else {
                    s += expr_str(a.value.get(), depth + 1);
                }
                s += "\n";
            }
            s += ind(depth) + "}";
            return s;
        }
    }
    return "?";
}

void stmt_str(std::string& out, const Stmt* s, int depth) {
    out += ind(depth);
    switch (s->kind) {
        case Stmt::Kind::let_:
            out += std::string(s->is_var ? "var " : "let ") + s->name + ": " +
                   type_str(s->type.get());
            if (s->init) out += " = " + expr_str(s->init.get(), depth);
            out += "\n";
            break;
        case Stmt::Kind::assign:
            out += "assign " + expr_str(s->target.get(), depth) + " " +
                   to_string(s->op) + " " + expr_str(s->value.get(), depth) + "\n";
            break;
        case Stmt::Kind::expr:
            out += expr_str(s->expr.get(), depth) + "\n";
            break;
        case Stmt::Kind::ret:
            out += "return";
            if (s->expr) out += " " + expr_str(s->expr.get(), depth);
            out += "\n";
            break;
        case Stmt::Kind::brk: out += "break\n"; break;
        case Stmt::Kind::cont: out += "continue\n"; break;
        case Stmt::Kind::defer_:
            out += "defer " + expr_str(s->expr.get(), depth) + "\n";
            break;
        case Stmt::Kind::unsafe_:
            out += "unsafe " + block_str(s->body, depth) + "\n";
            break;
        case Stmt::Kind::if_:
            out += "if " + expr_str(s->cond.get(), depth) + " " +
                   block_str(s->body, depth);
            if (!s->else_body.empty()) {
                out += " else ";
                if (s->else_body.size() == 1 &&
                    s->else_body[0]->kind == Stmt::Kind::if_) {
                    std::string nested;
                    stmt_str(nested, s->else_body[0].get(), depth);
                    // splice: drop the indent so it reads `else if ...`
                    out += nested.substr(ind(depth).size());
                    return;
                }
                out += block_str(s->else_body, depth);
            }
            out += "\n";
            break;
        case Stmt::Kind::for_ever:
            out += "for " + block_str(s->body, depth) + "\n";
            break;
        case Stmt::Kind::for_while:
            out += "for " + expr_str(s->cond.get(), depth) + " " +
                   block_str(s->body, depth) + "\n";
            break;
        case Stmt::Kind::for_in:
            out += "for " + s->loop_var + ": " + type_str(s->loop_type.get()) +
                   " in " + expr_str(s->iterable.get(), depth) + " " +
                   block_str(s->body, depth) + "\n";
            break;
    }
}

std::string generics_str(const std::vector<GenericParam>& gs) {
    if (gs.empty()) return "";
    std::string s = "<";
    for (size_t i = 0; i < gs.size(); i++) {
        if (i) s += ", ";
        s += gs[i].name;
        if (!gs[i].bound.empty()) s += ": " + gs[i].bound;
    }
    return s + ">";
}

void fn_str(std::string& out, const FnDecl& f, int depth) {
    out += ind(depth);
    if (f.is_pub) out += "pub ";
    if (f.is_override) out += "override ";
    out += "fn " + f.name + generics_str(f.generics) +
           params_str(f.params, f.has_self);
    if (f.ret) out += " -> " + type_str(f.ret.get());
    if (f.has_body) out += " " + block_str(f.body, depth);
    else out += "   [signature]";
    out += "\n";
}

} // namespace

std::string dump(const Module& m) {
    std::string out;
    for (const ImportDecl& i : m.imports) {
        out += "import " + i.path;
        if (!i.alias.empty()) out += " as " + i.alias;
        out += "\n";
    }
    if (!m.imports.empty()) out += "\n";

    for (const auto& [kind, idx] : m.order) {
        switch (kind) {
            case Module::DeclKind::class_: {
                const ClassDecl& c = m.classes[idx];
                if (c.is_pub) out += "pub ";
                out += c.is_interface ? "interface " : "class ";
                out += c.name + generics_str(c.generics);
                if (!c.supers.empty()) {
                    out += " : ";
                    for (size_t i = 0; i < c.supers.size(); i++) {
                        if (i) out += ", ";
                        out += c.supers[i];
                    }
                }
                out += " {\n";
                for (const FieldDecl& f : c.fields) {
                    out += ind(1);
                    if (f.is_pub) out += "pub ";
                    out += f.name + ": " + type_str(f.type.get());
                    if (f.def) out += " = " + expr_str(f.def.get(), 1);
                    out += "\n";
                }
                for (const FnDecl& f : c.methods) fn_str(out, f, 1);
                out += "}\n\n";
                break;
            }
            case Module::DeclKind::enum_: {
                const EnumDecl& e = m.enums[idx];
                if (e.is_pub) out += "pub ";
                out += "enum " + e.name + generics_str(e.generics) + " {\n";
                for (const EnumVariant& v : e.variants) {
                    out += ind(1) + v.name;
                    if (!v.payload.empty()) out += params_str(v.payload, false);
                    out += "\n";
                }
                for (const FnDecl& f : e.methods) fn_str(out, f, 1);
                out += "}\n\n";
                break;
            }
            case Module::DeclKind::fn:
                fn_str(out, m.fns[idx], 0);
                out += "\n";
                break;
        }
    }
    return out;
}

} // namespace beans
