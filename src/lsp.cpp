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
#include <set>
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

// Methods on the scalar types (int/float/decimal). These live inline in the
// checker rather than the builtin registry, so this mirrors that set — keep it
// in sync with the numeric branches of Checker::member_of.
struct ScalarMethod {
    const char* type;
    const char* name;
    const char* sig;
    const char* doc;
};
const std::vector<ScalarMethod>& scalar_methods() {
    static const std::vector<ScalarMethod> t = {
        {"int", "abs", "fn abs() -> int", "Absolute value."},
        {"float", "abs", "fn abs() -> float", "Absolute value."},
        {"float", "round", "fn round() -> int", "Nearest integer (rounds half away from zero)."},
        {"decimal", "abs", "fn abs() -> decimal", "Absolute value."},
        {"decimal", "round", "fn round(places: int) -> decimal", "Rounded to `places` decimal places."},
    };
    return t;
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
    const FnDecl* fn = nullptr;      // enclosing fn (Ident) for local/param lookup
    const ClassDecl* cls = nullptr;  // enclosing class (self / method context)
    const Expr* recv = nullptr;      // receiver expr for a Member hit
    const Expr* expr = nullptr;      // the leaf expr the cursor landed on
    Found decl;                      // pre-rendered result when kind == Decl
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

void find_in_expr(const Expr* e, Cursor cur, const FnDecl* fn,
                  const ClassDecl* cls, Hit& out);

void find_in_stmt(const Stmt* s, Cursor cur, const FnDecl* fn,
                  const ClassDecl* cls, Hit& out) {
    if (!s) return;
    // a `let`/`var` binding name (the stmt's line/col is the name token)
    if (s->kind == Stmt::Kind::let_ && !s->name.empty() &&
        within(cur, s->line, s->col, s->line,
               s->col + static_cast<uint32_t>(s->name.size()))) {
        out.kind = Hit::Ident; // resolved as a local via the enclosing fn
        out.name = s->name;
        out.fn = fn;
        out.cls = cls;
        return;
    }
    find_in_type(s->type.get(), cur, out);
    find_in_type(s->loop_type.get(), cur, out);
    find_in_expr(s->init.get(), cur, fn, cls, out);
    find_in_expr(s->target.get(), cur, fn, cls, out);
    find_in_expr(s->value.get(), cur, fn, cls, out);
    find_in_expr(s->expr.get(), cur, fn, cls, out);
    find_in_expr(s->cond.get(), cur, fn, cls, out);
    find_in_expr(s->iterable.get(), cur, fn, cls, out);
    for (const auto& b : s->body) find_in_stmt(b.get(), cur, fn, cls, out);
    for (const auto& b : s->else_body) find_in_stmt(b.get(), cur, fn, cls, out);
}

void find_in_expr(const Expr* e, Cursor cur, const FnDecl* fn,
                  const ClassDecl* cls, Hit& out) {
    if (!e) return;
    // descend into children first, so the innermost node wins
    find_in_expr(e->lhs.get(), cur, fn, cls, out);
    find_in_expr(e->rhs.get(), cur, fn, cls, out);
    find_in_expr(e->callee.get(), cur, fn, cls, out);
    for (const auto& a : e->args) find_in_expr(a.get(), cur, fn, cls, out);
    find_in_expr(e->object.get(), cur, fn, cls, out);
    find_in_expr(e->index_expr.get(), cur, fn, cls, out);
    for (const auto& en : e->entries) {
        find_in_expr(en.key.get(), cur, fn, cls, out);
        find_in_expr(en.value.get(), cur, fn, cls, out);
    }
    find_in_type(e->type.get(), cur, out);
    for (const auto& t : e->type_args) find_in_type(t.get(), cur, out);
    for (const auto& b : e->body) find_in_stmt(b.get(), cur, fn, cls, out);
    find_in_expr(e->cond.get(), cur, fn, cls, out);
    find_in_expr(e->then_e.get(), cur, fn, cls, out);
    find_in_expr(e->else_e.get(), cur, fn, cls, out);
    find_in_expr(e->subject.get(), cur, fn, cls, out);
    for (const auto& arm : e->arms) {
        find_in_expr(arm.value.get(), cur, fn, cls, out);
        for (const auto& b : arm.body) find_in_stmt(b.get(), cur, fn, cls, out);
    }
    if (out.kind != Hit::None) return; // a deeper node already claimed the cursor

    if (e->kind == Expr::Kind::ident &&
        within(cur, e->line, e->col, e->end_line, e->end_col)) {
        out.kind = Hit::Ident;
        out.name = std::string(e->text);
        out.fn = fn;
        out.cls = cls;
        out.expr = e;
    } else if (e->kind == Expr::Kind::field && !e->name.empty() &&
               e->end_col >= static_cast<uint32_t>(e->name.size())) {
        // the member name occupies [end_col - name.size(), end_col) on end_line
        uint32_t nsc = e->end_col - static_cast<uint32_t>(e->name.size());
        if (within(cur, e->end_line, nsc, e->end_line, e->end_col)) {
            out.kind = Hit::Member;
            out.name = e->name;
            out.fn = fn;
            out.cls = cls;
            out.recv = e->object.get();
            out.expr = e;
        }
    } else if ((e->kind == Expr::Kind::new_ || e->kind == Expr::Kind::init) &&
               !e->name.empty() &&
               within(cur, e->line, e->col, e->end_line, e->end_col)) {
        // `new Point(...)` / `Point { ... }` — the type name
        out.kind = Hit::Type;
        out.name = e->name;
        out.expr = e;
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
        for (const auto& s : f.body) find_in_stmt(s.get(), cur, &f, nullptr, hit);
        if (hit.kind != Hit::None) return hit;
    }
    auto walk_methods = [&](const std::vector<FnDecl>& ms, const ClassDecl* owner) {
        for (const FnDecl& f : ms) {
            for (const Param& p : f.params) find_in_type(p.type.get(), cur, hit);
            find_in_type(f.ret.get(), cur, hit);
            for (const auto& s : f.body) find_in_stmt(s.get(), cur, &f, owner, hit);
            if (hit.kind != Hit::None) return;
        }
    };
    for (const ClassDecl& c : mod.classes) {
        for (const FieldDecl& fld : c.fields) find_in_type(fld.type.get(), cur, hit);
        if (hit.kind != Hit::None) return hit;
        walk_methods(c.methods, &c);
        if (hit.kind != Hit::None) return hit;
    }
    for (const EnumDecl& en : mod.enums) {
        walk_methods(en.methods, nullptr);
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
                   b.doc ? b.doc : "", "builtin", false};
            return true;
        }
    for (const BuiltinMethod& b : builtin_methods())
        if (word == b.name) {
            out = {true, "builtin method",
                   "fn " + bt_name(b.recv) + "." + b.name + bt_params(b.params) +
                       bt_ret(b.ret),
                   b.doc ? b.doc : "", "builtin", false};
            return true;
        }
    for (const BuiltinStatic& b : builtin_statics())
        if (word == b.name) {
            out = {true, "builtin static",
                   "static fn " + std::string(b.cls) + "." + b.name +
                       bt_params(b.params) + bt_ret(b.ret),
                   b.doc ? b.doc : "", "builtin", false};
            return true;
        }
    return false;
}

// ---- declared-type resolution (query-facade internals) -------------------

const FnDecl* find_fn(const Program& prog, const std::string& name) {
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files)
            for (const FnDecl& f : pf->mod.fns)
                if (f.name == name) return &f;
    return nullptr;
}

const ClassDecl* find_class(const Program& prog, const std::string& name) {
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files)
            for (const ClassDecl& c : pf->mod.classes)
                if (c.name == name) return &c;
    return nullptr;
}

const EnumDecl* find_enum(const Program& prog, const std::string& name) {
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files)
            for (const EnumDecl& e : pf->mod.enums)
                if (e.name == name) return &e;
    return nullptr;
}

// head name of a written type: `List<T>` -> "List", `Point` -> "Point"
std::string type_head(const TypeRef* t) {
    return (t && t->kind == TypeRef::Kind::named) ? t->name : "";
}

// scan a fn's (nested) let/var bindings and for-in vars for `name`
std::string local_type_head_in(const std::vector<StmtPtr>& body,
                               const std::string& name) {
    for (const StmtPtr& s : body) {
        if (s->kind == Stmt::Kind::let_ && s->name == name)
            return type_head(s->type.get());
        if (s->kind == Stmt::Kind::for_in && s->loop_var == name)
            return type_head(s->loop_type.get());
        std::string h = local_type_head_in(s->body, name);
        if (!h.empty()) return h;
        h = local_type_head_in(s->else_body, name);
        if (!h.empty()) return h;
    }
    return "";
}

// declared type head of a field/method named `member` on `type_name` (+ supers)
std::string member_type_head(const Program& prog, const std::string& type_name,
                             const std::string& member, int depth) {
    if (depth > 8) return "";
    if (const ClassDecl* c = find_class(prog, type_name)) {
        for (const FieldDecl& f : c->fields)
            if (f.name == member) return type_head(f.type.get());
        for (const FnDecl& m : c->methods)
            if (m.name == member) return type_head(m.ret.get());
        if (!c->base.empty()) {
            std::string h = member_type_head(prog, c->base, member, depth + 1);
            if (!h.empty()) return h;
        }
        for (const std::string& i : c->interfaces) {
            std::string h = member_type_head(prog, i, member, depth + 1);
            if (!h.empty()) return h;
        }
    }
    if (const EnumDecl* en = find_enum(prog, type_name))
        for (const FnDecl& m : en->methods)
            if (m.name == member) return type_head(m.ret.get());
    return "";
}

// best-effort declared type head of an expression
std::string type_name_of(const Program& prog, const FnDecl* fn,
                         const ClassDecl* cls, const Expr* e) {
    if (!e) return "";
    switch (e->kind) {
        case Expr::Kind::self_ref: return cls ? cls->name : "";
        case Expr::Kind::new_:
        case Expr::Kind::init: return e->name;
        case Expr::Kind::int_lit: return "int";
        case Expr::Kind::float_lit: return "float";
        case Expr::Kind::string_lit: return "string";
        case Expr::Kind::bool_lit: return "bool";
        case Expr::Kind::cast: return type_head(e->type.get());
        case Expr::Kind::ident: {
            std::string n(e->text);
            if (fn) {
                for (const Param& p : fn->params)
                    if (p.name == n) return type_head(p.type.get());
                std::string h = local_type_head_in(fn->body, n);
                if (!h.empty()) return h;
            }
            if (cls)
                for (const FieldDecl& f : cls->fields)
                    if (f.name == n) return type_head(f.type.get());
            return "";
        }
        case Expr::Kind::field: {
            std::string recv = type_name_of(prog, fn, cls, e->object.get());
            return recv.empty() ? "" : member_type_head(prog, recv, e->name, 0);
        }
        case Expr::Kind::call: {
            const Expr* c = e->callee.get();
            if (c && c->kind == Expr::Kind::field) {
                std::string recv = type_name_of(prog, fn, cls, c->object.get());
                if (!recv.empty()) return member_type_head(prog, recv, c->name, 0);
            } else if (c && c->kind == Expr::Kind::ident) {
                if (const FnDecl* f = find_fn(prog, std::string(c->text)))
                    return type_head(f->ret.get());
            }
            return "";
        }
        default: return "";
    }
}

// find a member named `member` on `type_name` (+ supers) and render a hover
bool find_member_of(const Program& prog, const std::string& type_name,
                    const std::string& member, Found& out, int depth = 0) {
    if (depth > 8) return false;
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files) {
            for (const ClassDecl& c : pf->mod.classes) {
                if (c.name != type_name) continue;
                for (const FnDecl& m : c.methods)
                    if (m.name == member) {
                        out = {true, "method", method_sig(c.name, m), m.doc,
                               loc(pf->path, m.line), true};
                        return true;
                    }
                for (const FieldDecl& f : c.fields)
                    if (f.name == member) {
                        out = {true, "field", c.name + "." + field_sig(f), f.doc,
                               loc(pf->path, f.line), true};
                        return true;
                    }
                if (!c.base.empty() &&
                    find_member_of(prog, c.base, member, out, depth + 1))
                    return true;
                for (const std::string& i : c.interfaces)
                    if (find_member_of(prog, i, member, out, depth + 1)) return true;
            }
            for (const EnumDecl& e : pf->mod.enums) {
                if (e.name != type_name) continue;
                for (const FnDecl& m : e.methods)
                    if (m.name == member) {
                        out = {true, "method", method_sig(e.name, m), m.doc,
                               loc(pf->path, m.line), true};
                        return true;
                    }
            }
        }
    // scalar (int/float/decimal) methods
    for (const ScalarMethod& s : scalar_methods())
        if (member == s.name && type_name == s.type) {
            // s.sig begins with "fn "; splice the type in after it
            out = {true, "method", "fn " + type_name + "." + std::string(s.sig + 3),
                   s.doc, "builtin", false};
            return true;
        }
    // builtin type (string/Bytes/...) methods
    for (const BuiltinMethod& b : builtin_methods())
        if (member == b.name && bt_name(b.recv) == type_name) {
            out = {true, "builtin method",
                   "fn " + bt_name(b.recv) + "." + b.name + bt_params(b.params) +
                       bt_ret(b.ret),
                   b.doc ? b.doc : "", "builtin", false};
            return true;
        }
    return false;
}

// the declaration of a method named `member` on `type_name` (+ supers)
const FnDecl* find_method_decl(const Program& prog, const std::string& type_name,
                               const std::string& member, int depth = 0) {
    if (depth > 8) return nullptr;
    if (const ClassDecl* c = find_class(prog, type_name)) {
        for (const FnDecl& m : c->methods)
            if (m.name == member) return &m;
        if (!c->base.empty())
            if (const FnDecl* r = find_method_decl(prog, c->base, member, depth + 1))
                return r;
        for (const std::string& i : c->interfaces)
            if (const FnDecl* r = find_method_decl(prog, i, member, depth + 1))
                return r;
    }
    if (const EnumDecl* e = find_enum(prog, type_name))
        for (const FnDecl& m : e->methods)
            if (m.name == member) return &m;
    return nullptr;
}

// ---- enclosing-call finder (signature help) ------------------------------

bool pos_ge(Cursor p, uint32_t l, uint32_t c) {
    return p.line > l || (p.line == l && p.col >= c);
}

struct CallHit {
    const Expr* call = nullptr;
    const FnDecl* fn = nullptr;
    const ClassDecl* cls = nullptr;
};

void find_call(const Expr* e, Cursor cur, const FnDecl* fn, const ClassDecl* cls,
               CallHit& out);

void find_call_stmt(const Stmt* s, Cursor cur, const FnDecl* fn,
                    const ClassDecl* cls, CallHit& out) {
    if (!s) return;
    find_call(s->init.get(), cur, fn, cls, out);
    find_call(s->target.get(), cur, fn, cls, out);
    find_call(s->value.get(), cur, fn, cls, out);
    find_call(s->expr.get(), cur, fn, cls, out);
    find_call(s->cond.get(), cur, fn, cls, out);
    find_call(s->iterable.get(), cur, fn, cls, out);
    for (const auto& b : s->body) find_call_stmt(b.get(), cur, fn, cls, out);
    for (const auto& b : s->else_body) find_call_stmt(b.get(), cur, fn, cls, out);
}

void find_call(const Expr* e, Cursor cur, const FnDecl* fn, const ClassDecl* cls,
               CallHit& out) {
    if (!e) return;
    find_call(e->lhs.get(), cur, fn, cls, out);
    find_call(e->rhs.get(), cur, fn, cls, out);
    find_call(e->callee.get(), cur, fn, cls, out);
    for (const auto& a : e->args) find_call(a.get(), cur, fn, cls, out);
    find_call(e->object.get(), cur, fn, cls, out);
    find_call(e->index_expr.get(), cur, fn, cls, out);
    for (const auto& en : e->entries) {
        find_call(en.key.get(), cur, fn, cls, out);
        find_call(en.value.get(), cur, fn, cls, out);
    }
    for (const auto& b : e->body) find_call_stmt(b.get(), cur, fn, cls, out);
    find_call(e->cond.get(), cur, fn, cls, out);
    find_call(e->then_e.get(), cur, fn, cls, out);
    find_call(e->else_e.get(), cur, fn, cls, out);
    find_call(e->subject.get(), cur, fn, cls, out);
    for (const auto& arm : e->arms) {
        find_call(arm.value.get(), cur, fn, cls, out);
        for (const auto& b : arm.body) find_call_stmt(b.get(), cur, fn, cls, out);
    }
    if (out.call) return; // a deeper call already claimed the cursor

    bool inside = within(cur, e->line, e->col, e->end_line, e->end_col);
    if (inside && e->kind == Expr::Kind::call) {
        const Expr* callee = e->callee.get();
        // only when the cursor is in the argument region, past the callee
        if (callee && pos_ge(cur, callee->end_line, callee->end_col))
            out = {e, fn, cls};
    } else if (inside && e->kind == Expr::Kind::new_) {
        out = {e, fn, cls};
    }
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
            // precise: resolve the member on the receiver's declared type
            if (hit.recv) {
                std::string t = type_name_of(prog, hit.fn, hit.cls, hit.recv);
                if (!t.empty() && find_member_of(prog, t, hit.name, f))
                    return format(f);
            }
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

// ---- public query facade --------------------------------------------------

namespace {

void collect_members(const Program& prog, const std::string& type_name,
                     std::vector<MemberInfo>& out, std::set<std::string>& seen,
                     int depth) {
    if (depth > 8) return;
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files) {
            for (const ClassDecl& c : pf->mod.classes) {
                if (c.name != type_name) continue;
                for (const FieldDecl& f : c.fields)
                    if (seen.insert(f.name).second)
                        out.push_back({f.name, "field", field_sig(f), f.doc,
                                       loc(pf->path, f.line)});
                for (const FnDecl& m : c.methods)
                    if (seen.insert(m.name).second)
                        out.push_back({m.name, "method", fn_sig(m), m.doc,
                                       loc(pf->path, m.line)});
                if (!c.base.empty())
                    collect_members(prog, c.base, out, seen, depth + 1);
                for (const std::string& i : c.interfaces)
                    collect_members(prog, i, out, seen, depth + 1);
                return;
            }
            for (const EnumDecl& e : pf->mod.enums) {
                if (e.name != type_name) continue;
                for (const EnumVariant& v : e.variants)
                    if (seen.insert(v.name).second) {
                        std::string sig = v.name;
                        if (!v.payload.empty()) sig += params_str(v.payload, false);
                        out.push_back({v.name, "variant", sig, v.doc,
                                       loc(pf->path, e.line)});
                    }
                for (const FnDecl& m : e.methods)
                    if (seen.insert(m.name).second)
                        out.push_back({m.name, "method", fn_sig(m), m.doc,
                                       loc(pf->path, m.line)});
                return;
            }
        }
    for (const ScalarMethod& s : scalar_methods())
        if (type_name == s.type && seen.insert(s.name).second)
            out.push_back({s.name, "method", s.sig, s.doc, "builtin"});
    for (const BuiltinMethod& b : builtin_methods())
        if (bt_name(b.recv) == type_name && seen.insert(b.name).second)
            out.push_back({std::string(b.name), "method",
                           "fn " + std::string(b.name) + bt_params(b.params) +
                               bt_ret(b.ret),
                           b.doc ? b.doc : "", "builtin"});
}

void collect_locals(const std::vector<StmtPtr>& body, std::vector<ScopeName>& out) {
    for (const StmtPtr& s : body) {
        if (s->kind == Stmt::Kind::let_ && !s->name.empty()) {
            std::string sig = std::string(s->is_var ? "var " : "let ") + s->name;
            if (s->type) sig += ": " + type_str(s->type.get());
            out.push_back({s->name, "local", sig, ""});
        }
        if (s->kind == Stmt::Kind::for_in && !s->loop_var.empty()) {
            std::string sig = s->loop_var;
            if (s->loop_type) sig += ": " + type_str(s->loop_type.get());
            out.push_back({s->loop_var, "local", sig, ""});
        }
        collect_locals(s->body, out);
        collect_locals(s->else_body, out);
    }
}

const FnDecl* enclosing_fn(const Module& mod, Cursor cur, const ClassDecl** cls_out) {
    const FnDecl* fn = nullptr;
    for (const FnDecl& f : mod.fns)
        if (within(cur, f.line, f.col, f.end_line, f.end_col)) fn = &f;
    for (const ClassDecl& c : mod.classes)
        for (const FnDecl& m : c.methods)
            if (within(cur, m.line, m.col, m.end_line, m.end_col)) {
                fn = &m;
                *cls_out = &c;
            }
    for (const EnumDecl& e : mod.enums)
        for (const FnDecl& m : e.methods)
            if (within(cur, m.line, m.col, m.end_line, m.end_col)) fn = &m;
    return fn;
}

} // namespace

std::vector<MemberInfo> members_of(const Program& prog, const std::string& type_name) {
    std::vector<MemberInfo> out;
    std::set<std::string> seen;
    collect_members(prog, type_name, out, seen, 0);
    return out;
}

std::string type_at(const Program& prog, const std::string& file, uint32_t line,
                    uint32_t col) {
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return "";
    Hit hit = find_in_module(cur->path, cur->mod, {line, col});
    if (hit.expr) return type_name_of(prog, hit.fn, hit.cls, hit.expr);
    return "";
}

std::vector<ScopeName> scope_at(const Program& prog, const std::string& file,
                                uint32_t line, uint32_t col) {
    std::vector<ScopeName> out;
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return out;
    const ClassDecl* cls = nullptr;
    const FnDecl* fn = enclosing_fn(cur->mod, {line, col}, &cls);
    if (fn) {
        for (const Param& p : fn->params) {
            std::string sig = p.name;
            if (p.type) sig += ": " + type_str(p.type.get());
            out.push_back({p.name, "parameter", sig, ""});
        }
        collect_locals(fn->body, out);
    }
    if (cls)
        for (const FieldDecl& f : cls->fields)
            out.push_back({f.name, "field", field_sig(f), f.doc});
    for (const FnDecl& f : cur->mod.fns)
        out.push_back({f.name, "function", fn_sig(f), f.doc});
    for (const ClassDecl& c : cur->mod.classes)
        out.push_back({c.name, "type", class_sig(c), c.doc});
    for (const EnumDecl& e : cur->mod.enums)
        out.push_back({e.name, "enum", enum_sig(e), e.doc});
    static const char* kws[] = {
        "let", "var", "fn",     "if",     "else",   "for",    "in",
        "match", "return", "break", "continue", "class", "enum", "interface",
        "struct", "union", "import", "pub", "override", "static", "defer",
        "unsafe", "self", "true", "false", "new", "as"};
    for (const char* kw : kws) out.push_back({kw, "keyword", "", ""});
    return out;
}

SignatureInfo signature_at(const Program& prog, const std::string& file,
                           uint32_t line, uint32_t col) {
    SignatureInfo si;
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return si;
    Cursor cursor{line, col};

    CallHit hit;
    for (const FnDecl& f : cur->mod.fns) {
        for (const auto& s : f.body) find_call_stmt(s.get(), cursor, &f, nullptr, hit);
        if (hit.call) break;
    }
    if (!hit.call)
        for (const ClassDecl& c : cur->mod.classes) {
            for (const FnDecl& m : c.methods)
                for (const auto& s : m.body)
                    find_call_stmt(s.get(), cursor, &m, &c, hit);
            if (hit.call) break;
        }
    if (!hit.call)
        for (const EnumDecl& e : cur->mod.enums) {
            for (const FnDecl& m : e.methods)
                for (const auto& s : m.body)
                    find_call_stmt(s.get(), cursor, &m, nullptr, hit);
            if (hit.call) break;
        }
    if (!hit.call) return si;

    const Expr* call = hit.call;
    const FnDecl* target = nullptr;
    std::string name;
    bool ctor = false;
    if (call->kind == Expr::Kind::new_) {
        ctor = true;
        name = call->name;
        target = find_method_decl(prog, call->name, "init");
    } else {
        const Expr* callee = call->callee.get();
        if (callee && callee->kind == Expr::Kind::ident) {
            name = std::string(callee->text);
            target = find_fn(prog, name);
        } else if (callee && callee->kind == Expr::Kind::field) {
            name = callee->name;
            std::string recv = type_name_of(prog, hit.fn, hit.cls, callee->object.get());
            if (!recv.empty()) target = find_method_decl(prog, recv, name);
        }
    }

    // active parameter: how many arguments are already complete before the cursor
    int active = 0;
    for (const auto& a : call->args) {
        if (within(cursor, a->line, a->col, a->end_line, a->end_col)) break;
        if (pos_ge(cursor, a->end_line, a->end_col)) active++;
        else break;
    }
    si.active = active;

    if (target) {
        for (const Param& p : target->params) {
            std::string pl = p.name;
            if (p.type) pl += ": " + type_str(p.type.get());
            si.params.push_back(pl);
        }
        std::string label = (ctor ? "new " : "") + name + "(";
        for (size_t i = 0; i < si.params.size(); i++) {
            if (i) label += ", ";
            label += si.params[i];
        }
        label += ")";
        if (target->ret) label += " -> " + type_str(target->ret.get());
        si.label = label;
        si.doc = target->doc;
        if (!si.params.empty() && si.active >= static_cast<int>(si.params.size()))
            si.active = static_cast<int>(si.params.size()) - 1;
        si.ok = true;
    } else if (!name.empty()) {
        si.label = (ctor ? "new " : "") + name + "(…)";
        si.ok = true;
    }
    return si;
}

std::string render_doc_markdown(const std::string& raw) { return render_doc(raw); }

// ---- definition / references / outline internals -------------------------

namespace {

void set_def(DefLoc& d, const std::string& path, uint32_t nl, uint32_t nc,
             size_t len) {
    d.ok = true;
    d.file = path;
    d.line = nl;
    d.col = nc;
    d.end_line = nl;
    d.end_col = nc + static_cast<uint32_t>(len);
}

bool fn_def(const Program& prog, const std::string& name, DefLoc& d) {
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files)
            for (const FnDecl& f : pf->mod.fns)
                if (f.name == name && f.name_line) {
                    set_def(d, pf->path, f.name_line, f.name_col, name.size());
                    return true;
                }
    return false;
}

bool type_def(const Program& prog, const std::string& name, DefLoc& d) {
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files) {
            for (const ClassDecl& c : pf->mod.classes)
                if (c.name == name && c.name_line) {
                    set_def(d, pf->path, c.name_line, c.name_col, name.size());
                    return true;
                }
            for (const EnumDecl& e : pf->mod.enums)
                if (e.name == name && e.name_line) {
                    set_def(d, pf->path, e.name_line, e.name_col, name.size());
                    return true;
                }
        }
    return false;
}

bool member_def(const Program& prog, const std::string& type_name,
                const std::string& member, DefLoc& d, int depth = 0) {
    if (depth > 8) return false;
    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files) {
            for (const ClassDecl& c : pf->mod.classes) {
                if (c.name != type_name) continue;
                for (const FnDecl& m : c.methods)
                    if (m.name == member && m.name_line) {
                        set_def(d, pf->path, m.name_line, m.name_col, member.size());
                        return true;
                    }
                for (const FieldDecl& f : c.fields)
                    if (f.name == member && f.name_line) {
                        set_def(d, pf->path, f.name_line, f.name_col, member.size());
                        return true;
                    }
                if (!c.base.empty() && member_def(prog, c.base, member, d, depth + 1))
                    return true;
                for (const std::string& i : c.interfaces)
                    if (member_def(prog, i, member, d, depth + 1)) return true;
            }
            for (const EnumDecl& e : pf->mod.enums) {
                if (e.name != type_name) continue;
                for (const FnDecl& m : e.methods)
                    if (m.name == member && m.name_line) {
                        set_def(d, pf->path, m.name_line, m.name_col, member.size());
                        return true;
                    }
            }
        }
    return false;
}

bool local_def(const FnDecl* fn, const std::string& name, const std::string& path,
               DefLoc& d);

bool local_def_stmts(const std::vector<StmtPtr>& body, const std::string& name,
                     const std::string& path, DefLoc& d) {
    for (const StmtPtr& s : body) {
        if (s->kind == Stmt::Kind::let_ && s->name == name) {
            set_def(d, path, s->line, s->col, name.size());
            return true;
        }
        if (s->kind == Stmt::Kind::for_in && s->loop_var == name) {
            set_def(d, path, s->line, s->col, name.size());
            return true;
        }
        if (local_def_stmts(s->body, name, path, d)) return true;
        if (local_def_stmts(s->else_body, name, path, d)) return true;
    }
    return false;
}

bool local_def(const FnDecl* fn, const std::string& name, const std::string& path,
               DefLoc& d) {
    if (!fn) return false;
    for (const Param& p : fn->params)
        if (p.name == name) {
            set_def(d, path, p.line, p.col, name.size());
            return true;
        }
    return local_def_stmts(fn->body, name, path, d);
}

// if the cursor sits on a declaration's name, return that definition
bool decl_name_at(const std::string& path, const Module& mod, Cursor cur,
                  DefLoc& d) {
    auto on = [&](uint32_t nl, uint32_t nc, const std::string& nm) {
        return nl && within(cur, nl, nc, nl, nc + static_cast<uint32_t>(nm.size()));
    };
    for (const FnDecl& f : mod.fns)
        if (on(f.name_line, f.name_col, f.name)) {
            set_def(d, path, f.name_line, f.name_col, f.name.size());
            return true;
        }
    for (const ClassDecl& c : mod.classes) {
        if (on(c.name_line, c.name_col, c.name)) {
            set_def(d, path, c.name_line, c.name_col, c.name.size());
            return true;
        }
        for (const FnDecl& m : c.methods)
            if (on(m.name_line, m.name_col, m.name)) {
                set_def(d, path, m.name_line, m.name_col, m.name.size());
                return true;
            }
        for (const FieldDecl& f : c.fields)
            if (on(f.name_line, f.name_col, f.name)) {
                set_def(d, path, f.name_line, f.name_col, f.name.size());
                return true;
            }
    }
    for (const EnumDecl& e : mod.enums) {
        if (on(e.name_line, e.name_col, e.name)) {
            set_def(d, path, e.name_line, e.name_col, e.name.size());
            return true;
        }
        for (const FnDecl& m : e.methods)
            if (on(m.name_line, m.name_col, m.name)) {
                set_def(d, path, m.name_line, m.name_col, m.name.size());
                return true;
            }
        for (const EnumVariant& v : e.variants)
            if (on(v.line, v.col, v.name)) {
                set_def(d, path, v.line, v.col, v.name.size());
                return true;
            }
    }
    return false;
}

} // namespace

std::vector<Completion> completions_at(const Program& prog, const std::string& file,
                                       uint32_t line, uint32_t col) {
    std::vector<Completion> out;
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return out;
    const std::string& text = cur->source;

    size_t p = offset_of(text, line, col);
    if (p > text.size()) p = text.size();
    size_t q = p;
    while (q > 0 && is_ident_char(text[q - 1])) q--; // skip the partial word
    std::string prefix = text.substr(q, p - q);
    auto matches = [&](const std::string& n) {
        return prefix.empty() ||
               (n.size() >= prefix.size() && n.compare(0, prefix.size(), prefix) == 0);
    };

    if (q > 0 && text[q - 1] == '.') {
        // member completion: resolve the receiver just before the '.'
        if (col > prefix.size() + 2) {
            uint32_t recv_col = col - static_cast<uint32_t>(prefix.size()) - 2;
            std::string ty = type_at(prog, file, line, recv_col);
            if (!ty.empty())
                for (const MemberInfo& m : members_of(prog, ty))
                    if (matches(m.name))
                        out.push_back({m.name, m.kind, m.signature, m.doc});
        }
        return out;
    }

    // scope completion: names visible here
    for (const ScopeName& s : scope_at(prog, file, line, col))
        if (matches(s.name))
            out.push_back({s.name, s.kind, s.signature, s.doc});
    return out;
}

DefLoc definition_at(const Program& prog, const std::string& file, uint32_t line,
                     uint32_t col) {
    DefLoc d;
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return d;
    Cursor cursor{line, col};
    if (decl_name_at(cur->path, cur->mod, cursor, d)) return d; // on a definition

    Hit hit = find_in_module(cur->path, cur->mod, cursor);
    switch (hit.kind) {
        case Hit::Type:
            type_def(prog, hit.name, d);
            break;
        case Hit::Member: {
            std::string t;
            if (hit.recv) t = type_name_of(prog, hit.fn, hit.cls, hit.recv);
            if (!t.empty()) member_def(prog, t, hit.name, d);
            break;
        }
        case Hit::Ident:
            if (!local_def(hit.fn, hit.name, cur->path, d) &&
                !fn_def(prog, hit.name, d))
                type_def(prog, hit.name, d);
            break;
        default:
            break;
    }
    return d;
}

// ---- references ----------------------------------------------------------

namespace {

void refs_type(const TypeRef* t, const std::string& name, const std::string& path,
               std::vector<RefLoc>& out) {
    if (!t) return;
    for (const auto& a : t->args) refs_type(a.get(), name, path, out);
    for (const auto& a : t->fn_params) refs_type(a.get(), name, path, out);
    refs_type(t->fn_ret.get(), name, path, out);
    refs_type(t->array_elem.get(), name, path, out);
    if (t->kind == TypeRef::Kind::named && t->name == name && t->line)
        out.push_back({path, t->line, t->col, t->line,
                       t->col + static_cast<uint32_t>(name.size())});
}

void refs_expr(const Expr* e, const std::string& name, const std::string& path,
               std::vector<RefLoc>& out);

void refs_stmt(const Stmt* s, const std::string& name, const std::string& path,
               std::vector<RefLoc>& out) {
    if (!s) return;
    if (s->kind == Stmt::Kind::let_ && s->name == name && s->line)
        out.push_back({path, s->line, s->col, s->line,
                       s->col + static_cast<uint32_t>(name.size())});
    refs_type(s->type.get(), name, path, out);
    refs_type(s->loop_type.get(), name, path, out);
    refs_expr(s->init.get(), name, path, out);
    refs_expr(s->target.get(), name, path, out);
    refs_expr(s->value.get(), name, path, out);
    refs_expr(s->expr.get(), name, path, out);
    refs_expr(s->cond.get(), name, path, out);
    refs_expr(s->iterable.get(), name, path, out);
    for (const auto& b : s->body) refs_stmt(b.get(), name, path, out);
    for (const auto& b : s->else_body) refs_stmt(b.get(), name, path, out);
}

void refs_expr(const Expr* e, const std::string& name, const std::string& path,
               std::vector<RefLoc>& out) {
    if (!e) return;
    if (e->kind == Expr::Kind::ident && e->text == name && e->line)
        out.push_back({path, e->line, e->col, e->line,
                       e->col + static_cast<uint32_t>(name.size())});
    if (e->kind == Expr::Kind::field && e->name == name && e->end_col >= name.size())
        out.push_back({path, e->end_line,
                       e->end_col - static_cast<uint32_t>(name.size()), e->end_line,
                       e->end_col});
    if ((e->kind == Expr::Kind::new_ || e->kind == Expr::Kind::init) &&
        e->name == name && e->line)
        out.push_back({path, e->line, e->col, e->line,
                       e->col + static_cast<uint32_t>(name.size())});
    refs_expr(e->lhs.get(), name, path, out);
    refs_expr(e->rhs.get(), name, path, out);
    refs_expr(e->callee.get(), name, path, out);
    for (const auto& a : e->args) refs_expr(a.get(), name, path, out);
    refs_expr(e->object.get(), name, path, out);
    refs_expr(e->index_expr.get(), name, path, out);
    for (const auto& en : e->entries) {
        refs_expr(en.key.get(), name, path, out);
        refs_expr(en.value.get(), name, path, out);
    }
    refs_type(e->type.get(), name, path, out);
    for (const auto& t : e->type_args) refs_type(t.get(), name, path, out);
    for (const auto& b : e->body) refs_stmt(b.get(), name, path, out);
    refs_expr(e->cond.get(), name, path, out);
    refs_expr(e->then_e.get(), name, path, out);
    refs_expr(e->else_e.get(), name, path, out);
    refs_expr(e->subject.get(), name, path, out);
    for (const auto& arm : e->arms) {
        refs_expr(arm.value.get(), name, path, out);
        for (const auto& b : arm.body) refs_stmt(b.get(), name, path, out);
    }
}

void refs_fn(const FnDecl& f, const std::string& name, const std::string& path,
             std::vector<RefLoc>& out) {
    if (f.name == name && f.name_line)
        out.push_back({path, f.name_line, f.name_col, f.name_line,
                       f.name_col + static_cast<uint32_t>(name.size())});
    for (const Param& p : f.params) {
        if (p.name == name && p.line)
            out.push_back({path, p.line, p.col, p.line,
                           p.col + static_cast<uint32_t>(name.size())});
        refs_type(p.type.get(), name, path, out);
    }
    refs_type(f.ret.get(), name, path, out);
    for (const auto& s : f.body) refs_stmt(s.get(), name, path, out);
}

} // namespace

std::vector<RefLoc> references_at(const Program& prog, const std::string& file,
                                  uint32_t line, uint32_t col) {
    std::vector<RefLoc> out;
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return out;
    Hit hit = find_in_module(cur->path, cur->mod, {line, col});
    std::string name = hit.name.empty() ? word_at(cur->source, line, col) : hit.name;
    if (name.empty()) return out;

    for (const auto& pkg : prog.packages)
        for (const auto& pf : pkg->files) {
            const std::string& path = pf->path;
            for (const FnDecl& f : pf->mod.fns) refs_fn(f, name, path, out);
            for (const ClassDecl& c : pf->mod.classes) {
                if (c.name == name && c.name_line)
                    out.push_back({path, c.name_line, c.name_col, c.name_line,
                                   c.name_col + static_cast<uint32_t>(name.size())});
                for (const FieldDecl& fl : c.fields) {
                    if (fl.name == name && fl.name_line)
                        out.push_back({path, fl.name_line, fl.name_col, fl.name_line,
                                       fl.name_col + static_cast<uint32_t>(name.size())});
                    refs_type(fl.type.get(), name, path, out);
                }
                for (const FnDecl& m : c.methods) refs_fn(m, name, path, out);
            }
            for (const EnumDecl& e : pf->mod.enums) {
                if (e.name == name && e.name_line)
                    out.push_back({path, e.name_line, e.name_col, e.name_line,
                                   e.name_col + static_cast<uint32_t>(name.size())});
                for (const EnumVariant& v : e.variants)
                    if (v.name == name && v.line)
                        out.push_back({path, v.line, v.col, v.line,
                                       v.col + static_cast<uint32_t>(name.size())});
                for (const FnDecl& m : e.methods) refs_fn(m, name, path, out);
            }
        }
    return out;
}

// ---- document symbols ----------------------------------------------------

std::vector<DocSymbol> document_symbols(const Program& prog, const std::string& file) {
    std::vector<DocSymbol> out;
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return out;
    const Module& mod = cur->mod;

    auto sym = [](const std::string& name, const std::string& detail, int kind,
                  uint32_t l, uint32_t c, uint32_t el, uint32_t ec, uint32_t nl,
                  uint32_t nc) {
        DocSymbol s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.line = l ? l : 1;
        s.col = c ? c : 1;
        s.end_line = el ? el : s.line;
        s.end_col = ec ? ec : s.col;
        s.sel_line = nl ? nl : s.line;
        s.sel_col = nc ? nc : s.col;
        s.sel_end_line = s.sel_line;
        s.sel_end_col = s.sel_col + static_cast<uint32_t>(name.size());
        return s;
    };

    for (const auto& [kind, idx] : mod.order) {
        if (kind == Module::DeclKind::fn) {
            const FnDecl& f = mod.fns[idx];
            out.push_back(sym(f.name, fn_sig(f), 12, f.line, f.col, f.end_line,
                              f.end_col, f.name_line, f.name_col));
        } else if (kind == Module::DeclKind::class_) {
            const ClassDecl& c = mod.classes[idx];
            int k = c.is_interface ? 11 : c.is_struct ? 23 : 5; // Interface/Struct/Class
            DocSymbol s = sym(c.name, class_sig(c), k, c.line, c.col, c.end_line,
                              c.end_col, c.name_line, c.name_col);
            for (const FieldDecl& f : c.fields)
                s.children.push_back(sym(f.name, field_sig(f), 8, f.line, f.col,
                                         f.end_line, f.end_col, f.name_line,
                                         f.name_col));
            for (const FnDecl& m : c.methods)
                s.children.push_back(sym(m.name, fn_sig(m), 6, m.line, m.col,
                                         m.end_line, m.end_col, m.name_line,
                                         m.name_col));
            out.push_back(std::move(s));
        } else {
            const EnumDecl& e = mod.enums[idx];
            DocSymbol s = sym(e.name, enum_sig(e), 10, e.line, e.col, e.end_line,
                              e.end_col, e.name_line, e.name_col);
            for (const EnumVariant& v : e.variants)
                s.children.push_back(sym(v.name, v.name, 22, v.line, v.col,
                                         v.end_line, v.end_col, v.line, v.col));
            for (const FnDecl& m : e.methods)
                s.children.push_back(sym(m.name, fn_sig(m), 6, m.line, m.col,
                                         m.end_line, m.end_col, m.name_line,
                                         m.name_col));
            out.push_back(std::move(s));
        }
    }
    return out;
}

} // namespace beans
