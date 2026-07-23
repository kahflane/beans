// Hover rendering for the language server (and the `lsp-probe` command).
//
// Given a position, it produces the Markdown an editor would show: a signature
// built live from the AST (or the builtin registry) plus the `///` doc block
// the lexer keeps. Nothing here is a hardcoded catalog; every signature and doc
// is read back out of the compiler's own data.
//
// Resolution is span-based: symbol_at() walks the AST to the innermost named
// node under the cursor and classifies it (a type, a member, a local/param, or
// a free name), so a type and a same-named function no longer collide, and
// `a.b` distinguishes the receiver from the member. Precise member-of-type
// resolution (what type is `a`?) still falls back to a name search until the
// checker query facade lands.

#include "lsp.h"

#include <cstddef>
#include <string_view>

#include "builtins.h"

namespace beans {

namespace {

bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// ---- rendering the written types back to text ----------------------------
// Mirrors ast_print.cpp's type_str/params_str, kept local so hover formatting
// can evolve independently of the AST dump.

std::string type_str(const TypeRef* t) {
    if (!t) return "?";
    if (t->kind == TypeRef::Kind::fixed_array) {
        return "[" + type_str(t->array_elem.get()) + "; " +
               std::to_string(t->array_len) + "]";
    }
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

std::string params_str(const std::vector<Param>& ps, bool self_first) {
    std::string s = "(";
    bool first = true;
    if (self_first) { s += "self"; first = false; }
    for (const Param& p : ps) {
        if (!first) s += ", ";
        first = false;
        if (p.passing == Param::Passing::move) s += "move ";
        if (p.passing == Param::Passing::inout) s += "inout ";
        s += p.name;
        if (p.type) s += ": " + type_str(p.type.get());
    }
    s += ")";
    return s;
}

std::string generics_str(const std::vector<GenericParam>& gs) {
    if (gs.empty()) return "";
    std::string s = "<";
    for (size_t i = 0; i < gs.size(); i++) {
        if (i) s += ", ";
        s += gs[i].name;
        if (!gs[i].bounds.empty()) {
            s += " implements ";
            for (size_t j = 0; j < gs[i].bounds.size(); j++) {
                if (j) s += " & ";
                s += gs[i].bounds[j];
            }
        }
    }
    return s + ">";
}

// ---- signature strings ---------------------------------------------------

std::string fn_sig(const FnDecl& f) {
    std::string s;
    if (f.is_pub) s += "pub ";
    if (f.is_static) s += "static ";
    s += "fn " + f.name + generics_str(f.generics) +
         params_str(f.params, f.has_self);
    if (f.ret) s += " -> " + type_str(f.ret.get());
    return s;
}

// a method, qualified by its owning type: `fn Point.norm2(self) -> int`
std::string method_sig(const std::string& owner, const FnDecl& f) {
    std::string s;
    if (f.is_pub) s += "pub ";
    if (f.is_override) s += "override ";
    if (f.is_static) s += "static ";
    s += "fn " + owner + "." + f.name + generics_str(f.generics) +
         params_str(f.params, f.has_self);
    if (f.ret) s += " -> " + type_str(f.ret.get());
    return s;
}

std::string class_sig(const ClassDecl& c) {
    std::string s;
    if (c.is_pub) s += "pub ";
    if (c.is_move_only) s += "unique ";
    s += c.is_interface ? "interface "
         : c.is_union    ? "union "
         : c.is_struct   ? "struct "
                         : "class ";
    s += c.name + generics_str(c.generics);
    if (!c.base.empty()) s += " extends " + c.base;
    if (!c.interfaces.empty()) {
        s += c.is_interface ? " extends " : " implements ";
        for (size_t i = 0; i < c.interfaces.size(); i++) {
            if (i) s += ", ";
            s += c.interfaces[i];
        }
    }
    return s;
}

std::string enum_sig(const EnumDecl& e) {
    std::string s;
    if (e.is_pub) s += "pub ";
    s += "enum " + e.name + generics_str(e.generics) + " {";
    for (size_t i = 0; i < e.variants.size(); i++) {
        s += (i ? ", " : " ") + e.variants[i].name;
        if (!e.variants[i].payload.empty())
            s += params_str(e.variants[i].payload, false);
    }
    s += e.variants.empty() ? "}" : " }";
    return s;
}

std::string field_sig(const FieldDecl& f) {
    std::string s;
    if (f.is_pub) s += "pub ";
    s += f.name + ": " + type_str(f.type.get());
    return s;
}

// ---- doc block -> Markdown ------------------------------------------------
// The stored block is the raw source span, `///` prefixes and all. Strip the
// leading indentation, the marker, and one optional space per line.

std::string render_doc(std::string_view raw) {
    std::string out;
    size_t i = 0;
    bool first = true;
    while (i <= raw.size()) {
        size_t nl = raw.find('\n', i);
        std::string_view line =
            (nl == std::string_view::npos) ? raw.substr(i) : raw.substr(i, nl - i);
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) s++;
        line.remove_prefix(s);
        if (line.substr(0, 3) == "///") line.remove_prefix(3);
        if (!line.empty() && line[0] == ' ') line.remove_prefix(1);
        if (!first) out += '\n';
        out += std::string(line);
        first = false;
        if (nl == std::string_view::npos) break;
        i = nl + 1;
    }
    return out;
}

// ---- builtin registry types ----------------------------------------------

std::string bt_name(BT t) {
    switch (t) {
        case BT::unit:         return "unit";
        case BT::i64:          return "int";
        case BT::f64:          return "float";
        case BT::dec:          return "decimal";
        case BT::boolean:      return "bool";
        case BT::str:          return "string";
        case BT::bytes:        return "Bytes";
        case BT::file:         return "File";
        case BT::mmap:         return "Mmap";
        case BT::self_recv:    return "Self";
        case BT::opt_i64:      return "Option<int>";
        case BT::opt_str:      return "Option<string>";
        case BT::list_str:     return "List<string>";
        case BT::res_i64:      return "Result<int>";
        case BT::res_f64:      return "Result<float>";
        case BT::res_dec:      return "Result<decimal>";
        case BT::res_str:      return "Result<string>";
        case BT::res_bool:     return "Result<bool>";
        case BT::res_bytes:    return "Result<Bytes>";
        case BT::res_file:     return "Result<File>";
        case BT::res_mmap:     return "Result<Mmap>";
        case BT::res_list_str: return "Result<List<string>>";
    }
    return "?";
}

std::string bt_params(const std::vector<BT>& ps) {
    std::string s = "(";
    for (size_t i = 0; i < ps.size(); i++) {
        if (i) s += ", ";
        s += bt_name(ps[i]);
    }
    return s + ")";
}

std::string bt_ret(BT ret) {
    if (ret == BT::unit) return "";
    return " -> " + bt_name(ret);
}

// ---- the assembled hover --------------------------------------------------

struct Found {
    bool ok = false;
    std::string kind;
    std::string sig;
    std::string doc;   // raw `///` span for user decls; plain text for builtins
    std::string where; // "path:line" or "builtin"
    bool raw_doc = true;
};

std::string format(const Found& f) {
    std::string out = "```beans\n" + f.sig + "\n```";
    std::string doc = f.raw_doc ? render_doc(f.doc) : f.doc;
    if (!doc.empty()) out += "\n\n" + doc;
    out += "\n\n*" + f.kind + " · " + f.where + "*";
    return out;
}

std::string loc(const std::string& path, uint32_t line) {
    return path + ":" + std::to_string(line);
}

// What kind of declaration a name lookup is allowed to match. A cursor known
// to sit on a type never resolves to a same-named function, and vice versa.
enum class Want { any, type, fn, member };

// Search one module for a declaration named `word`, restricted to `want`.
bool match_in_module(const std::string& path, const Module& mod,
                     const std::string& word, Want want, Found& out) {
    if (want == Want::any || want == Want::fn) {
        for (const FnDecl& f : mod.fns) {
            if (f.name == word) {
                out = {true, "function", fn_sig(f), f.doc, loc(path, f.line), true};
                return true;
            }
        }
    }
    if (want == Want::any || want == Want::type) {
        for (const ClassDecl& c : mod.classes) {
            if (c.name == word) {
                const char* k = c.is_interface ? "interface"
                                : c.is_union    ? "union"
                                : c.is_struct   ? "struct"
                                                : "class";
                out = {true, k, class_sig(c), c.doc, loc(path, c.line), true};
                return true;
            }
        }
        for (const EnumDecl& e : mod.enums) {
            if (e.name == word) {
                out = {true, "enum", enum_sig(e), e.doc, loc(path, e.line), true};
                return true;
            }
        }
    }
    if (want == Want::any || want == Want::member) {
        for (const ClassDecl& c : mod.classes) {
            for (const FnDecl& m : c.methods) {
                if (m.name == word) {
                    out = {true, "method", method_sig(c.name, m), m.doc,
                           loc(path, m.line), true};
                    return true;
                }
            }
            for (const FieldDecl& fld : c.fields) {
                if (fld.name == word) {
                    out = {true, "field", c.name + "." + field_sig(fld), fld.doc,
                           loc(path, fld.line), true};
                    return true;
                }
            }
        }
        for (const EnumDecl& e : mod.enums) {
            for (const FnDecl& m : e.methods) {
                if (m.name == word) {
                    out = {true, "method", method_sig(e.name, m), m.doc,
                           loc(path, m.line), true};
                    return true;
                }
            }
            for (const EnumVariant& v : e.variants) {
                if (v.name == word) {
                    std::string sig = e.name + "." + v.name;
                    if (!v.payload.empty()) sig += params_str(v.payload, false);
                    out = {true, "variant", sig, v.doc, loc(path, e.line), true};
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- span-based cursor resolution ----------------------------------------

struct Cursor { uint32_t line, col; };

// sl:sc <= cursor < el:ec, with el==0 meaning "span not stamped"
bool within(Cursor p, uint32_t sl, uint32_t sc, uint32_t el, uint32_t ec) {
    if (el == 0) return false;
    bool after_start = p.line > sl || (p.line == sl && p.col >= sc);
    bool before_end = p.line < el || (p.line == el && p.col < ec);
    return after_start && before_end;
}

// The innermost named thing under the cursor.
struct Hit {
    enum K { None, Type, Member, Ident, Decl } kind = None;
    std::string name;
    const FnDecl* fn = nullptr; // enclosing fn (Ident) for local/param lookup
    Found decl;                 // pre-rendered result when kind == Decl
};

void find_in_type(const TypeRef* t, Cursor cur, Hit& out) {
    if (!t) return;
    for (const auto& a : t->args) find_in_type(a.get(), cur, out);
    for (const auto& a : t->fn_params) find_in_type(a.get(), cur, out);
    find_in_type(t->fn_ret.get(), cur, out);
    find_in_type(t->array_elem.get(), cur, out);
    if (t->kind == TypeRef::Kind::named && !t->name.empty() &&
        within(cur, t->line, t->col, t->end_line, t->end_col)) {
        // the head name only (args handled above); prefer the innermost already set
        if (out.kind == Hit::None) { out.kind = Hit::Type; out.name = t->name; }
    }
}

void find_in_expr(const Expr* e, Cursor cur, const FnDecl* fn, Hit& out);

void find_in_stmt(const Stmt* s, Cursor cur, const FnDecl* fn, Hit& out) {
    if (!s) return;
    // a `let`/`var` binding name (the stmt's line/col is the name token)
    if (s->kind == Stmt::Kind::let_ && !s->name.empty() &&
        within(cur, s->line, s->col, s->line,
               s->col + static_cast<uint32_t>(s->name.size()))) {
        out.kind = Hit::Ident; // resolved as a local via the enclosing fn
        out.name = s->name;
        out.fn = fn;
        return;
    }
    find_in_type(s->type.get(), cur, out);
    find_in_type(s->loop_type.get(), cur, out);
    find_in_expr(s->init.get(), cur, fn, out);
    find_in_expr(s->target.get(), cur, fn, out);
    find_in_expr(s->value.get(), cur, fn, out);
    find_in_expr(s->expr.get(), cur, fn, out);
    find_in_expr(s->cond.get(), cur, fn, out);
    find_in_expr(s->iterable.get(), cur, fn, out);
    for (const auto& b : s->body) find_in_stmt(b.get(), cur, fn, out);
    for (const auto& b : s->else_body) find_in_stmt(b.get(), cur, fn, out);
}

void find_in_expr(const Expr* e, Cursor cur, const FnDecl* fn, Hit& out) {
    if (!e) return;
    // descend into children first, so the innermost node wins
    find_in_expr(e->lhs.get(), cur, fn, out);
    find_in_expr(e->rhs.get(), cur, fn, out);
    find_in_expr(e->callee.get(), cur, fn, out);
    for (const auto& a : e->args) find_in_expr(a.get(), cur, fn, out);
    find_in_expr(e->object.get(), cur, fn, out);
    find_in_expr(e->index_expr.get(), cur, fn, out);
    for (const auto& en : e->entries) {
        find_in_expr(en.key.get(), cur, fn, out);
        find_in_expr(en.value.get(), cur, fn, out);
    }
    find_in_type(e->type.get(), cur, out);
    for (const auto& t : e->type_args) find_in_type(t.get(), cur, out);
    for (const auto& b : e->body) find_in_stmt(b.get(), cur, fn, out);
    find_in_expr(e->cond.get(), cur, fn, out);
    find_in_expr(e->then_e.get(), cur, fn, out);
    find_in_expr(e->else_e.get(), cur, fn, out);
    find_in_expr(e->subject.get(), cur, fn, out);
    for (const auto& arm : e->arms) {
        find_in_expr(arm.value.get(), cur, fn, out);
        for (const auto& b : arm.body) find_in_stmt(b.get(), cur, fn, out);
    }
    if (out.kind != Hit::None) return; // a deeper node already claimed the cursor

    if (e->kind == Expr::Kind::ident &&
        within(cur, e->line, e->col, e->end_line, e->end_col)) {
        out.kind = Hit::Ident;
        out.name = std::string(e->text);
        out.fn = fn;
    } else if (e->kind == Expr::Kind::field && !e->name.empty() &&
               e->end_col >= static_cast<uint32_t>(e->name.size())) {
        // the member name occupies [end_col - name.size(), end_col) on end_line
        uint32_t nsc = e->end_col - static_cast<uint32_t>(e->name.size());
        if (within(cur, e->end_line, nsc, e->end_line, e->end_col)) {
            out.kind = Hit::Member;
            out.name = e->name;
        }
    } else if ((e->kind == Expr::Kind::new_ || e->kind == Expr::Kind::init) &&
               !e->name.empty() &&
               within(cur, e->line, e->col, e->end_line, e->end_col)) {
        // `new Point(...)` / `Point { ... }` — the type name
        out.kind = Hit::Type;
        out.name = e->name;
    }
}

// scan a fn's params and its (possibly nested) `let`/`var` bindings for `name`
bool find_local_in_stmts(const std::vector<StmtPtr>& body, const std::string& name,
                         const std::string& path, Found& out) {
    for (const StmtPtr& s : body) {
        if (s->kind == Stmt::Kind::let_ && s->name == name) {
            std::string sig = std::string(s->is_var ? "var " : "let ") + s->name;
            if (s->type) sig += ": " + type_str(s->type.get());
            out = {true, "local", sig, "", loc(path, s->line), true};
            return true;
        }
        if (s->kind == Stmt::Kind::for_in && s->loop_var == name) {
            std::string sig = "for " + s->loop_var;
            if (s->loop_type) sig += ": " + type_str(s->loop_type.get());
            out = {true, "loop variable", sig, "", loc(path, s->line), true};
            return true;
        }
        if (find_local_in_stmts(s->body, name, path, out)) return true;
        if (find_local_in_stmts(s->else_body, name, path, out)) return true;
    }
    return false;
}

bool find_local(const FnDecl* fn, const std::string& name, const std::string& path,
                Found& out) {
    if (!fn) return false;
    for (const Param& p : fn->params) {
        if (p.name == name) {
            std::string sig = p.name;
            if (p.type) sig += ": " + type_str(p.type.get());
            out = {true, "parameter", sig, "", loc(path, p.line), true};
            return true;
        }
    }
    return find_local_in_stmts(fn->body, name, path, out);
}

// walk a module's declarations: first the decl names (cursor on a definition),
// then the bodies (cursor on a reference)
Hit find_in_module(const std::string& path, const Module& mod, Cursor cur) {
    Hit hit;
    auto name_hit = [&](uint32_t nl, uint32_t nc, const std::string& nm) {
        return nl && within(cur, nl, nc, nl, nc + static_cast<uint32_t>(nm.size()));
    };

    for (const FnDecl& f : mod.fns) {
        if (name_hit(f.name_line, f.name_col, f.name)) {
            hit.kind = Hit::Decl;
            hit.decl = {true, "function", fn_sig(f), f.doc, loc(path, f.line), true};
            return hit;
        }
    }
    for (const ClassDecl& c : mod.classes) {
        if (name_hit(c.name_line, c.name_col, c.name)) {
            const char* k = c.is_interface ? "interface" : c.is_union ? "union"
                            : c.is_struct  ? "struct"    : "class";
            hit.kind = Hit::Decl;
            hit.decl = {true, k, class_sig(c), c.doc, loc(path, c.line), true};
            return hit;
        }
        for (const FnDecl& m : c.methods)
            if (name_hit(m.name_line, m.name_col, m.name)) {
                hit.kind = Hit::Decl;
                hit.decl = {true, "method", method_sig(c.name, m), m.doc,
                            loc(path, m.line), true};
                return hit;
            }
        for (const FieldDecl& fld : c.fields)
            if (name_hit(fld.name_line, fld.name_col, fld.name)) {
                hit.kind = Hit::Decl;
                hit.decl = {true, "field", c.name + "." + field_sig(fld), fld.doc,
                            loc(path, fld.line), true};
                return hit;
            }
    }
    for (const EnumDecl& en : mod.enums) {
        if (name_hit(en.name_line, en.name_col, en.name)) {
            hit.kind = Hit::Decl;
            hit.decl = {true, "enum", enum_sig(en), en.doc, loc(path, en.line), true};
            return hit;
        }
        for (const FnDecl& m : en.methods)
            if (name_hit(m.name_line, m.name_col, m.name)) {
                hit.kind = Hit::Decl;
                hit.decl = {true, "method", method_sig(en.name, m), m.doc,
                            loc(path, m.line), true};
                return hit;
            }
        for (const EnumVariant& v : en.variants)
            if (name_hit(v.line, v.col, v.name)) {
                std::string sig = en.name + "." + v.name;
                if (!v.payload.empty()) sig += params_str(v.payload, false);
                hit.kind = Hit::Decl;
                hit.decl = {true, "variant", sig, v.doc, loc(path, en.line), true};
                return hit;
            }
    }

    // bodies: types on signatures, then statements/expressions
    for (const FnDecl& f : mod.fns) {
        for (const Param& p : f.params) find_in_type(p.type.get(), cur, hit);
        find_in_type(f.ret.get(), cur, hit);
        for (const auto& s : f.body) find_in_stmt(s.get(), cur, &f, hit);
        if (hit.kind != Hit::None) return hit;
    }
    auto walk_methods = [&](const std::vector<FnDecl>& ms) {
        for (const FnDecl& f : ms) {
            for (const Param& p : f.params) find_in_type(p.type.get(), cur, hit);
            find_in_type(f.ret.get(), cur, hit);
            for (const auto& s : f.body) find_in_stmt(s.get(), cur, &f, hit);
            if (hit.kind != Hit::None) return;
        }
    };
    for (const ClassDecl& c : mod.classes) {
        for (const FieldDecl& fld : c.fields) find_in_type(fld.type.get(), cur, hit);
        if (hit.kind != Hit::None) return hit;
        walk_methods(c.methods);
        if (hit.kind != Hit::None) return hit;
    }
    for (const EnumDecl& en : mod.enums) {
        walk_methods(en.methods);
        if (hit.kind != Hit::None) return hit;
    }
    return hit;
}

// ---- word fallback (no span hit) -----------------------------------------

size_t offset_of(std::string_view src, uint32_t line, uint32_t col) {
    size_t off = 0;
    uint32_t ln = 1;
    while (ln < line && off < src.size()) {
        if (src[off] == '\n') ln++;
        off++;
    }
    return off + (col - 1);
}

std::string word_at(std::string_view src, uint32_t line, uint32_t col) {
    if (line == 0 || col == 0) return "";
    size_t p = offset_of(src, line, col);
    if (p >= src.size() || !is_ident_char(src[p])) {
        if (p > 0 && p <= src.size() && is_ident_char(src[p - 1])) p--;
        else return "";
    }
    size_t a = p, b = p;
    while (a > 0 && is_ident_char(src[a - 1])) a--;
    while (b < src.size() && is_ident_char(src[b])) b++;
    return std::string(src.substr(a, b - a));
}

const PFile* find_pfile(const Program& prog, const std::string& file) {
    const PFile* by_suffix = nullptr;
    for (const auto& pkg : prog.packages) {
        for (const auto& pf : pkg->files) {
            if (pf->path == file) return pf.get();
            if (pf->path.size() >= file.size() &&
                pf->path.compare(pf->path.size() - file.size(), file.size(),
                                 file) == 0) {
                by_suffix = pf.get();
            }
        }
    }
    return by_suffix;
}

// look a name up across the program (cursor's file first), restricted to `want`
bool resolve_name(const Program& prog, const PFile* cur, const std::string& name,
                  Want want, Found& out) {
    if (cur && match_in_module(cur->path, cur->mod, name, want, out)) return true;
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files) {
            if (pf.get() == cur) continue;
            if (match_in_module(pf->path, pf->mod, name, want, out)) return true;
        }
    return false;
}

bool match_builtin(const std::string& word, Found& out) {
    for (const BuiltinFn& b : builtin_fns())
        if (word == b.name) {
            out = {true, "builtin function",
                   "fn " + std::string(b.module) + "." + b.name +
                       bt_params(b.params) + bt_ret(b.ret),
                   "", "builtin", false};
            return true;
        }
    for (const BuiltinMethod& b : builtin_methods())
        if (word == b.name) {
            out = {true, "builtin method",
                   "fn " + bt_name(b.recv) + "." + b.name + bt_params(b.params) +
                       bt_ret(b.ret),
                   "", "builtin", false};
            return true;
        }
    for (const BuiltinStatic& b : builtin_statics())
        if (word == b.name) {
            out = {true, "builtin static",
                   "static fn " + std::string(b.cls) + "." + b.name +
                       bt_params(b.params) + bt_ret(b.ret),
                   "", "builtin", false};
            return true;
        }
    return false;
}

} // namespace

std::string hover_at(const Program& prog, const std::string& file,
                     uint32_t line, uint32_t col) {
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return "";

    Hit hit = find_in_module(cur->path, cur->mod, {line, col});
    Found f;
    switch (hit.kind) {
        case Hit::Decl:
            return format(hit.decl);
        case Hit::Type:
            if (resolve_name(prog, cur, hit.name, Want::type, f)) return format(f);
            break;
        case Hit::Member:
            if (resolve_name(prog, cur, hit.name, Want::member, f)) return format(f);
            if (match_builtin(hit.name, f)) return format(f);
            break;
        case Hit::Ident:
            if (find_local(hit.fn, hit.name, cur->path, f)) return format(f);
            if (resolve_name(prog, cur, hit.name, Want::fn, f)) return format(f);
            if (resolve_name(prog, cur, hit.name, Want::type, f)) return format(f);
            break;
        case Hit::None:
            break;
    }

    // fallback: whatever word sits under the cursor, any kind
    std::string word = word_at(cur->source, line, col);
    if (!word.empty()) {
        if (resolve_name(prog, cur, word, Want::any, f)) return format(f);
        if (match_builtin(word, f)) return format(f);
    }
    return "";
}

} // namespace beans
