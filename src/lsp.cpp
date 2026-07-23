// Hover rendering for the `lsp-probe` command — the first slice of the beans
// language server. Given a position, it produces the Markdown an editor would
// show: a signature built live from the AST (or the builtin registry) plus the
// `///` doc block the lexer now keeps. Nothing here is a hardcoded catalog;
// every signature and doc is read back out of the compiler's own data.

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

// Search one module for a top-level or member declaration named `word`.
// Order puts types and free functions ahead of members so the common cases
// (hovering a function or type name) win over an unrelated same-named member.
bool match_in_module(const std::string& path, const Module& mod,
                     const std::string& word, Found& out) {
    for (const FnDecl& f : mod.fns) {
        if (f.name == word) {
            out = {true, "function", fn_sig(f), f.doc, loc(path, f.line), true};
            return true;
        }
    }
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
    // members, qualified by their owner so the hover says where they live
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
    return false;
}

bool match_builtin(const std::string& word, Found& out) {
    for (const BuiltinFn& b : builtin_fns()) {
        if (word == b.name) {
            out = {true, "builtin function",
                   "fn " + std::string(b.module) + "." + b.name +
                       bt_params(b.params) + bt_ret(b.ret),
                   "", "builtin", false};
            return true;
        }
    }
    for (const BuiltinMethod& b : builtin_methods()) {
        if (word == b.name) {
            out = {true, "builtin method",
                   "fn " + bt_name(b.recv) + "." + b.name + bt_params(b.params) +
                       bt_ret(b.ret),
                   "", "builtin", false};
            return true;
        }
    }
    for (const BuiltinStatic& b : builtin_statics()) {
        if (word == b.name) {
            out = {true, "builtin static",
                   "static fn " + std::string(b.cls) + "." + b.name +
                       bt_params(b.params) + bt_ret(b.ret),
                   "", "builtin", false};
            return true;
        }
    }
    return false;
}

// find the byte offset of a 1-based (line, col) inside source
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
        // allow the cursor to sit just past the end of a word
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
            // tolerate ./foo.b vs foo.b and absolute-vs-relative mismatches
            if (pf->path.size() >= file.size() &&
                pf->path.compare(pf->path.size() - file.size(), file.size(),
                                 file) == 0) {
                by_suffix = pf.get();
            }
        }
    }
    return by_suffix;
}

} // namespace

std::string hover_at(const Program& prog, const std::string& file,
                     uint32_t line, uint32_t col) {
    const PFile* cur = find_pfile(prog, file);
    if (!cur) return "";
    std::string word = word_at(cur->source, line, col);
    if (word.empty()) return "";

    Found f;
    // the cursor's own file first, so a local decl wins over a same-named one
    if (match_in_module(cur->path, cur->mod, word, f)) return format(f);
    for (const auto& pkg : prog.packages) {
        for (const auto& pf : pkg->files) {
            if (pf.get() == cur) continue;
            if (match_in_module(pf->path, pf->mod, word, f)) return format(f);
        }
    }
    if (match_builtin(word, f)) return format(f);
    return "";
}

} // namespace beans
