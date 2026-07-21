#include "codegen.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include "lexer.h"
#include "parser.h"
#include "value.h" // for Decimal::parse at compile time

namespace beans {

namespace {

std::string i128_str(__int128 v) {
    bool neg = v < 0;
    unsigned __int128 u = neg ? static_cast<unsigned __int128>(-v)
                              : static_cast<unsigned __int128>(v);
    std::string s;
    if (u == 0) s = "0";
    while (u > 0) {
        s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(u % 10)));
        u /= 10;
    }
    return neg ? "-" + s : s;
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

std::string fmt_double(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(bits));
    return buf;
}

std::string clean_number(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

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

// ---- semantic types for codegen ---------------------------------------------

struct Ty {
    enum K { i64_, f64_, i1_, str_, unit_, dec_, obj_, enum_, list_, bad_ };
    K k = K::bad_;
    std::string name;          // obj: impl/iface name, enum_: enum name
    std::vector<Ty*> args;     // enum_ args; list_ elem in args[0]
    const ClassDecl* iface = nullptr; // obj typed as an interface
};

namespace {
const char* ll(const Ty* t) {
    switch (t->k) {
        case Ty::i64_: return "i64";
        case Ty::f64_: return "double";
        case Ty::i1_: return "i1";
        case Ty::unit_: return "void";
        default: return "ptr";
    }
}
bool is_ref(const Ty* t) {
    switch (t->k) {
        case Ty::str_: case Ty::dec_: case Ty::obj_: case Ty::enum_: case Ty::list_:
            return true;
        default:
            return false;
    }
}
} // namespace

// a monomorphized class
struct CImpl {
    const ClassDecl* decl = nullptr;
    std::map<std::string, Ty*> env;
    std::string mangled;
    int id = 0;
    CImpl* parent = nullptr;
    struct FieldInfo {
        std::string name;
        Ty* ty;
        const FieldDecl* decl;
        int offset;
    };
    std::vector<FieldInfo> fields;
};

// ---- codegen implementation --------------------------------------------------

struct CG2 {
    const Module& mod;
    std::vector<CGError>& errors;

    std::vector<std::unique_ptr<Ty>> ty_pool;
    std::map<std::string, Ty*> ty_map;
    std::vector<std::unique_ptr<CImpl>> impls;
    std::map<std::string, CImpl*> impl_by_name;
    std::vector<CImpl*> impl_queue;

    std::map<std::string, const ClassDecl*> class_decls; // classes + interfaces
    std::map<std::string, const EnumDecl*> enum_decls;
    std::map<std::string, const FnDecl*> fn_decls;

    std::map<std::string, int> selectors;
    std::string globals;
    std::string fn_text;
    int next_str = 0;

    explicit CG2(const Module& m, std::vector<CGError>& errs) : mod(m), errors(errs) {
        for (const ClassDecl& c : m.classes) class_decls[c.name] = &c;
        for (const EnumDecl& e : m.enums) enum_decls[e.name] = &e;
        for (const FnDecl& f : m.fns) fn_decls[f.name] = &f;
    }

    void err(uint32_t line, uint32_t col, const std::string& msg) {
        errors.push_back({msg + " — not in the native backend yet (beansc run still works)",
                          line, col});
    }

    // ---- type interning ----
    Ty* intern(Ty t) {
        std::string key = std::to_string(t.k) + ":" + t.name;
        for (Ty* a : t.args) key += "," + std::to_string(reinterpret_cast<uintptr_t>(a));
        auto it = ty_map.find(key);
        if (it != ty_map.end()) return it->second;
        ty_pool.push_back(std::make_unique<Ty>(std::move(t)));
        ty_map[key] = ty_pool.back().get();
        return ty_pool.back().get();
    }
    Ty* prim(Ty::K k) { Ty t; t.k = k; return intern(std::move(t)); }
    Ty* t_i64() { return prim(Ty::i64_); }
    Ty* t_f64() { return prim(Ty::f64_); }
    Ty* t_bool() { return prim(Ty::i1_); }
    Ty* t_str() { return prim(Ty::str_); }
    Ty* t_unit() { return prim(Ty::unit_); }
    Ty* t_dec() { return prim(Ty::dec_); }
    Ty* t_bad() { return prim(Ty::bad_); }
    Ty* t_list(Ty* e) { Ty t; t.k = Ty::list_; t.args = {e}; return intern(std::move(t)); }
    Ty* t_enum(std::string n, std::vector<Ty*> a) {
        Ty t; t.k = Ty::enum_; t.name = std::move(n); t.args = std::move(a);
        return intern(std::move(t));
    }
    Ty* t_option(Ty* inner) { return t_enum("Option", {inner}); }
    Ty* t_error() { Ty t; t.k = Ty::obj_; t.name = "Error"; return intern(std::move(t)); }
    Ty* t_result(Ty* ok, Ty* e) { return t_enum("Result", {ok, e}); }
    Ty* t_obj(std::string n, const ClassDecl* iface = nullptr) {
        Ty t; t.k = Ty::obj_; t.name = std::move(n); t.iface = iface;
        return intern(std::move(t));
    }

    std::string mangle(Ty* t) {
        switch (t->k) {
            case Ty::i64_: return "int";
            case Ty::f64_: return "float";
            case Ty::i1_: return "bool";
            case Ty::str_: return "string";
            case Ty::dec_: return "decimal";
            case Ty::obj_: return t->name;
            case Ty::enum_: return t->name;
            case Ty::list_: return "List_" + mangle(t->args[0]);
            default: return "x";
        }
    }

    // ---- class instantiation ----
    CImpl* request_impl(const ClassDecl* decl, std::vector<Ty*> targs,
                        uint32_t line, uint32_t col) {
        std::string mangled = decl->name;
        for (Ty* a : targs) mangled += "$" + mangle(a);
        auto it = impl_by_name.find(mangled);
        if (it != impl_by_name.end()) return it->second;

        auto up = std::make_unique<CImpl>();
        CImpl* im = up.get();
        im->decl = decl;
        im->mangled = mangled;
        im->id = static_cast<int>(impls.size());
        for (size_t i = 0; i < decl->generics.size() && i < targs.size(); i++) {
            im->env[decl->generics[i].name] = targs[i];
        }
        impl_by_name[mangled] = im;
        impls.push_back(std::move(up));
        impl_queue.push_back(im);

        // parent chain (single class parent; interfaces carry no fields)
        for (const std::string& s : decl->supers) {
            auto cit = class_decls.find(s);
            if (cit != class_decls.end() && !cit->second->is_interface) {
                im->parent = request_impl(cit->second, {}, line, col);
            }
        }
        // fields: parent's first, then own
        if (im->parent) im->fields = im->parent->fields;
        for (const FieldDecl& f : decl->fields) {
            CImpl::FieldInfo fi;
            fi.name = f.name;
            fi.decl = &f;
            fi.ty = resolve(f.type.get(), im->env, f.line, f.col);
            fi.offset = 16 + 8 * static_cast<int>(im->fields.size());
            im->fields.push_back(std::move(fi));
        }
        return im;
    }

    // ---- type resolution ----
    Ty* resolve(const TypeRef* t, const std::map<std::string, Ty*>& env,
                uint32_t line, uint32_t col) {
        if (!t) return t_unit();
        if (t->kind == TypeRef::Kind::fn) {
            err(line, col, "function types");
            return t_bad();
        }
        const std::string& n = t->name;
        auto eit = env.find(n);
        if (eit != env.end()) return eit->second;

        if (n == "int" || n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
            n == "u8" || n == "u16" || n == "u32" || n == "u64" || n == "byte")
            return t_i64();
        if (n == "float" || n == "f32" || n == "f64") return t_f64();
        if (n == "bool") return t_bool();
        if (n == "string") return t_str();
        if (n == "decimal") return t_dec();
        if (n == "Error") return t_error();
        if (n == "List" && t->args.size() == 1)
            return t_list(resolve(t->args[0].get(), env, line, col));
        if (n == "Option" && t->args.size() == 1)
            return t_option(resolve(t->args[0].get(), env, line, col));
        if (n == "Result") {
            Ty* ok = t->args.empty() ? t_bad()
                                     : resolve(t->args[0].get(), env, line, col);
            Ty* e = t->args.size() >= 2 ? resolve(t->args[1].get(), env, line, col)
                                        : t_error();
            return t_result(ok, e);
        }
        auto cit = class_decls.find(n);
        if (cit != class_decls.end()) {
            if (cit->second->is_interface) return t_obj(n, cit->second);
            std::vector<Ty*> targs;
            for (const TypePtr& a : t->args)
                targs.push_back(resolve(a.get(), env, line, col));
            CImpl* im = request_impl(cit->second, std::move(targs), line, col);
            return t_obj(im->mangled);
        }
        auto enit = enum_decls.find(n);
        if (enit != enum_decls.end()) {
            if (!t->args.empty()) {
                err(line, col, "generic enums");
                return t_bad();
            }
            return t_enum(n, {});
        }
        err(line, col, "type '" + n + "'");
        return t_bad();
    }

    // ---- misc lookups ----
    std::string intern_string(const std::string& bytes) {
        std::string name = "@.str" + std::to_string(next_str++);
        globals += name + " = private unnamed_addr constant [" +
                   std::to_string(bytes.size() + 1) + " x i8] c\"";
        for (unsigned char c : bytes) {
            if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') {
                globals += static_cast<char>(c);
            } else {
                char buf[4];
                std::snprintf(buf, sizeof buf, "\\%02X", c);
                globals += buf;
            }
        }
        globals += "\\00\"\n";
        return name;
    }

    int selector(const std::string& name) {
        auto it = selectors.find(name);
        if (it != selectors.end()) return it->second;
        int id = static_cast<int>(selectors.size());
        selectors[name] = id;
        return id;
    }

    // find a method decl (with its env) starting from a class impl,
    // walking parent classes then interfaces (for signatures and defaults)
    struct FoundMethod {
        const FnDecl* decl = nullptr;
        const std::map<std::string, Ty*>* env = nullptr;
        std::string owner; // symbol owner: impl mangled name or interface name
        bool from_iface = false;
    };
    static const std::map<std::string, Ty*>& empty_env() {
        static const std::map<std::string, Ty*> e;
        return e;
    }
    FoundMethod find_method_class(CImpl* im, const std::string& name, bool want_body) {
        for (CImpl* c = im; c; c = c->parent) {
            for (const FnDecl& m : c->decl->methods) {
                if (m.name == name && m.has_self && (!want_body || m.has_body)) {
                    return {&m, &c->env, c->mangled, false};
                }
            }
        }
        // interfaces anywhere in the chain
        std::vector<const ClassDecl*> work;
        for (CImpl* c = im; c; c = c->parent) {
            for (const std::string& s : c->decl->supers) {
                auto cit = class_decls.find(s);
                if (cit != class_decls.end() && cit->second->is_interface)
                    work.push_back(cit->second);
            }
        }
        while (!work.empty()) {
            const ClassDecl* ic = work.back();
            work.pop_back();
            for (const FnDecl& m : ic->methods) {
                if (m.name == name && m.has_self && (!want_body || m.has_body)) {
                    return {&m, &empty_env(), ic->name, true};
                }
            }
            for (const std::string& s : ic->supers) {
                auto cit = class_decls.find(s);
                if (cit != class_decls.end()) work.push_back(cit->second);
            }
        }
        return {};
    }
    FoundMethod find_method_iface(const ClassDecl* iface, const std::string& name) {
        std::vector<const ClassDecl*> work = {iface};
        while (!work.empty()) {
            const ClassDecl* ic = work.back();
            work.pop_back();
            for (const FnDecl& m : ic->methods) {
                if (m.name == name && m.has_self) return {&m, &empty_env(), ic->name, true};
            }
            for (const std::string& s : ic->supers) {
                auto cit = class_decls.find(s);
                if (cit != class_decls.end()) work.push_back(cit->second);
            }
        }
        return {};
    }

    int variant_tag(const std::string& enum_name, const std::string& variant) {
        if (enum_name == "Option") return variant == "some" ? 0 : 1;
        if (enum_name == "Result") return variant == "ok" ? 0 : 1;
        auto it = enum_decls.find(enum_name);
        if (it == enum_decls.end()) return -1;
        int i = 0;
        for (const EnumVariant& v : it->second->variants) {
            if (v.name == variant) return i;
            i++;
        }
        return -1;
    }
    const EnumVariant* variant_decl(const std::string& enum_name,
                                    const std::string& variant) {
        auto it = enum_decls.find(enum_name);
        if (it == enum_decls.end()) return nullptr;
        for (const EnumVariant& v : it->second->variants) {
            if (v.name == variant) return &v;
        }
        return nullptr;
    }
};

// ---- per-function emitter ----------------------------------------------------

struct FnEmit {
    CG2& cg;
    const FnDecl& decl;
    std::string symbol;
    bool is_main;
    CImpl* self_impl;               // methods of a class
    const ClassDecl* self_iface;    // default methods of an interface
    const EnumDecl* self_enum;      // methods of an enum
    const std::map<std::string, Ty*>& env;

    std::string allocas, body;
    int next_reg = 0, next_bb = 0;
    bool terminated = false;
    struct Var { std::string slot; Ty* ty; };
    std::vector<std::map<std::string, Var>> scopes;
    std::vector<std::pair<std::string, std::string>> loop_stack;
    std::vector<std::unique_ptr<std::string>> interp_srcs;
    Ty* ret_ty = nullptr;

    FnEmit(CG2& cg, const FnDecl& d, std::string sym, bool main, CImpl* si,
           const ClassDecl* ifc, const EnumDecl* en, const std::map<std::string, Ty*>& e)
        : cg(cg), decl(d), symbol(std::move(sym)), is_main(main), self_impl(si),
          self_iface(ifc), self_enum(en), env(e) {}

    std::string reg() { return "%t" + std::to_string(next_reg++); }
    std::string bb() { return "bb" + std::to_string(next_bb++); }
    void line(const std::string& s) { body += "  " + s + "\n"; }
    void label(const std::string& l) { body += l + ":\n"; terminated = false; }
    void err(const Expr* e, const std::string& what) {
        cg.err(e ? e->line : decl.line, e ? e->col : decl.col, what);
    }

    Ty* rt(const TypeRef* t, uint32_t line, uint32_t col) {
        return cg.resolve(t, env, line, col);
    }

    Var* find_var(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }
    std::string alloc_slot(const std::string& name, Ty* t) {
        std::string slot = "%v" + std::to_string(next_reg++) + "." + name;
        allocas += "  " + slot + " = alloca " + ll(t) + "\n";
        scopes.back()[name] = {slot, t};
        return slot;
    }
    std::string fresh_slot(const char* tag, Ty* t) {
        std::string slot = "%v" + std::to_string(next_reg++) + "." + tag;
        allocas += "  " + slot + " = alloca " + ll(t) + "\n";
        return slot;
    }

    using EV = std::pair<std::string, Ty*>;

    // ---- allocation helpers ----
    std::string alloc_bytes(int n) {
        std::string r = reg();
        line(r + " = call ptr @beans_alloc(i64 " + std::to_string(n) + ")");
        return r;
    }
    void store_at(const std::string& base, int offset, const std::string& val, Ty* t) {
        std::string p = reg();
        line(p + " = getelementptr i8, ptr " + base + ", i64 " + std::to_string(offset));
        line("store " + std::string(ll(t)) + " " + val + ", ptr " + p);
    }
    std::string load_at(const std::string& base, int offset, Ty* t) {
        std::string p = reg(), r = reg();
        line(p + " = getelementptr i8, ptr " + base + ", i64 " + std::to_string(offset));
        line(r + " = load " + std::string(ll(t)) + ", ptr " + p);
        return r;
    }
    std::string box_enum(int tag, const std::vector<EV>& payload) {
        std::string b = alloc_bytes(8 + 8 * static_cast<int>(payload.size()));
        store_at(b, 0, std::to_string(tag), cg.t_i64());
        for (size_t i = 0; i < payload.size(); i++) {
            store_at(b, 8 + 8 * static_cast<int>(i), payload[i].first, payload[i].second);
        }
        return b;
    }

    // to raw i64 for list slots
    std::string to_slot(const EV& v) {
        if (v.second->k == Ty::i64_) return v.first;
        std::string r = reg();
        if (v.second->k == Ty::f64_) {
            line(r + " = bitcast double " + v.first + " to i64");
        } else if (v.second->k == Ty::i1_) {
            line(r + " = zext i1 " + v.first + " to i64");
        } else {
            line(r + " = ptrtoint ptr " + v.first + " to i64");
        }
        return r;
    }
    std::string from_slot(const std::string& v, Ty* t) {
        if (t->k == Ty::i64_) return v;
        std::string r = reg();
        if (t->k == Ty::f64_) line(r + " = bitcast i64 " + v + " to double");
        else if (t->k == Ty::i1_) line(r + " = trunc i64 " + v + " to i1");
        else line(r + " = inttoptr i64 " + v + " to ptr");
        return r;
    }

    // ---- expressions -------------------------------------------------------
    // (compiles the expression to IR; executes nothing)
    EV eval(const Expr* e, Ty* hint = nullptr) {
        switch (e->kind) {
            case Expr::Kind::int_lit: {
                if (hint && hint->k == Ty::dec_) return dec_literal(e->text);
                if (hint && hint->k == Ty::f64_)
                    return {fmt_double(parse_float_text(e->text)), cg.t_f64()};
                return {std::to_string(parse_int_text(e->text)), cg.t_i64()};
            }
            case Expr::Kind::float_lit: {
                if (hint && hint->k == Ty::dec_) return dec_literal(e->text);
                return {fmt_double(parse_float_text(e->text)), cg.t_f64()};
            }
            case Expr::Kind::bool_lit:
                return {e->bool_val ? "1" : "0", cg.t_bool()};
            case Expr::Kind::string_lit:
                return eval_string(e);
            case Expr::Kind::ident: {
                std::string name(e->text);
                if (Var* v = find_var(name)) {
                    std::string r = reg();
                    line(r + " = load " + std::string(ll(v->ty)) + ", ptr " + v->slot);
                    return {r, v->ty};
                }
                if (name == "none") {
                    Ty* inner = hint && hint->k == Ty::enum_ && !hint->args.empty()
                                    ? hint->args[0]
                                    : cg.t_i64();
                    std::string b = box_enum(1, {});
                    return {b, cg.t_option(inner)};
                }
                err(e, "reading '" + name + "'");
                return {"0", cg.t_i64()};
            }
            case Expr::Kind::self_ref: {
                if (Var* v = find_var("self")) {
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + v->slot);
                    return {r, v->ty};
                }
                err(e, "self");
                return {"null", cg.t_bad()};
            }
            case Expr::Kind::unary: {
                if (e->op == TokenKind::minus) {
                    EV v = eval(e->rhs.get(), hint);
                    std::string r = reg();
                    if (v.second->k == Ty::f64_) {
                        line(r + " = fneg double " + v.first);
                    } else if (v.second->k == Ty::dec_) {
                        line(r + " = call ptr @beans_dec_neg(ptr " + v.first + ")");
                    } else {
                        line(r + " = sub i64 0, " + v.first);
                    }
                    return {r, v.second};
                }
                if (e->op == TokenKind::bang) {
                    EV v = eval(e->rhs.get());
                    std::string r = reg();
                    line(r + " = xor i1 " + v.first + ", 1");
                    return {r, cg.t_bool()};
                }
                EV v = eval(e->rhs.get());
                std::string r = reg();
                line(r + " = xor i64 " + v.first + ", -1");
                return {r, cg.t_i64()};
            }
            case Expr::Kind::binary:
                return eval_binary(e);
            case Expr::Kind::range:
                err(e, "a range outside a for loop");
                return {"0", cg.t_i64()};
            case Expr::Kind::call:
                return eval_call(e, hint);
            case Expr::Kind::field:
                return eval_field(e);
            case Expr::Kind::index: {
                EV obj = eval(e->object.get());
                EV idx = eval(e->index_expr.get());
                if (obj.second->k != Ty::list_) {
                    err(e, "indexing this");
                    return {"0", cg.t_i64()};
                }
                // bounds-checked read
                Ty* elem = obj.second->args[0];
                std::string len = load_at(obj.first, 8, cg.t_i64());
                std::string okc = reg();
                line(okc + " = icmp ult i64 " + idx.first + ", " + len);
                std::string okb = bb(), badb = bb();
                line("br i1 " + okc + ", label %" + okb + ", label %" + badb);
                label(badb);
                std::string msg = cg.intern_string("list index out of range");
                line("call void @beans_panic(ptr " + msg + ", i64 " +
                     std::to_string(e->line) + ", i64 " + std::to_string(e->col) + ")");
                line("unreachable");
                label(okb);
                std::string data = load_at(obj.first, 0, cg.t_str()); // ptr
                std::string ep = reg(), raw = reg();
                line(ep + " = getelementptr i64, ptr " + data + ", i64 " + idx.first);
                line(raw + " = load i64, ptr " + ep);
                return {from_slot(raw, elem), elem};
            }
            case Expr::Kind::list_lit: {
                Ty* elem = hint && hint->k == Ty::list_ ? hint->args[0] : nullptr;
                std::string l = reg();
                line(l + " = call ptr @beans_list_new()");
                for (const ExprPtr& el : e->args) {
                    EV v = eval(el.get(), elem);
                    if (!elem) elem = v.second;
                    line("call void @beans_list_push(ptr " + l + ", i64 " + to_slot(v) + ")");
                }
                if (!elem) elem = cg.t_i64();
                return {l, cg.t_list(elem)};
            }
            case Expr::Kind::init:
                return eval_init(e, hint);
            case Expr::Kind::cast:
                return eval_cast(e);
            case Expr::Kind::try_: {
                EV v = eval(e->object.get());
                if (v.second->k != Ty::enum_) {
                    err(e, "? here");
                    return {"0", cg.t_i64()};
                }
                Ty* inner = v.second->args.empty() ? cg.t_i64() : v.second->args[0];
                std::string tag = load_at(v.first, 0, cg.t_i64());
                std::string c = reg();
                line(c + " = icmp eq i64 " + tag + ", 0");
                std::string okb = bb(), errb = bb();
                line("br i1 " + c + ", label %" + okb + ", label %" + errb);
                label(errb);
                line("ret ptr " + v.first); // pass the err/none box up unchanged
                label(okb);
                std::string payload = load_at(v.first, 8, inner);
                return {payload, inner};
            }
            case Expr::Kind::if_expr: {
                EV c = eval(e->cond.get());
                std::string then_bb = bb(), else_bb = bb(), end_bb = bb();
                line("br i1 " + c.first + ", label %" + then_bb + ", label %" + else_bb);
                label(then_bb);
                EV a = eval(e->then_e.get(), hint);
                std::string slot = fresh_slot("ifv", a.second);
                line("store " + std::string(ll(a.second)) + " " + a.first + ", ptr " + slot);
                line("br label %" + end_bb);
                label(else_bb);
                EV b2 = eval(e->else_e.get(), hint ? hint : a.second);
                line("store " + std::string(ll(a.second)) + " " + b2.first + ", ptr " + slot);
                line("br label %" + end_bb);
                label(end_bb);
                std::string r = reg();
                line(r + " = load " + std::string(ll(a.second)) + ", ptr " + slot);
                return {r, a.second};
            }
            case Expr::Kind::match_expr:
                return eval_match(e, hint);
            default:
                err(e, "this expression");
                return {"0", cg.t_i64()};
        }
    }

    EV dec_literal(std::string_view text) {
        Decimal d = Decimal::parse(clean_number(text));
        std::string r = reg();
        line(r + " = call ptr @beans_dec_new(i128 " + i128_str(d.coeff) + ", i64 " +
             std::to_string(d.scale) + ")");
        return {r, cg.t_dec()};
    }

    EV eval_string(const Expr* e) {
        std::vector<StrPiece> parts = split_interp(e->text, interp_srcs);
        if (parts.empty()) return {cg.intern_string(""), cg.t_str()};
        std::string acc;
        bool have = false;
        for (StrPiece& p : parts) {
            std::string piece;
            if (p.expr) {
                EV v = eval(p.expr.get());
                piece = to_str(v, p.expr.get());
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
        return {acc, cg.t_str()};
    }

    std::string to_str(const EV& v, const Expr* at) {
        std::string r = reg();
        switch (v.second->k) {
            case Ty::str_: return v.first;
            case Ty::i64_:
                line(r + " = call ptr @beans_from_int(i64 " + v.first + ")");
                return r;
            case Ty::f64_:
                line(r + " = call ptr @beans_from_float(double " + v.first + ")");
                return r;
            case Ty::dec_:
                line(r + " = call ptr @beans_dec_str(ptr " + v.first + ")");
                return r;
            case Ty::i1_: {
                std::string z = reg();
                line(z + " = zext i1 " + v.first + " to i32");
                line(r + " = call ptr @beans_from_bool(i32 " + z + ")");
                return r;
            }
            default:
                err(at, "printing this value");
                return cg.intern_string("?");
        }
    }

    EV eval_binary(const Expr* e) {
        if (e->op == TokenKind::andand || e->op == TokenKind::oror) {
            bool is_and = e->op == TokenKind::andand;
            std::string slot = fresh_slot("sc", cg.t_bool());
            EV l = eval(e->lhs.get());
            line("store i1 " + l.first + ", ptr " + slot);
            std::string more = bb(), end = bb();
            if (is_and) line("br i1 " + l.first + ", label %" + more + ", label %" + end);
            else line("br i1 " + l.first + ", label %" + end + ", label %" + more);
            label(more);
            EV r2 = eval(e->rhs.get());
            line("store i1 " + r2.first + ", ptr " + slot);
            line("br label %" + end);
            label(end);
            std::string r = reg();
            line(r + " = load i1, ptr " + slot);
            return {r, cg.t_bool()};
        }

        EV l = eval(e->lhs.get());
        EV r2 = eval(e->rhs.get(), l.second);
        if (l.second != r2.second &&
            (e->lhs->kind == Expr::Kind::int_lit || e->lhs->kind == Expr::Kind::float_lit)) {
            l = eval(e->lhs.get(), r2.second);
        }
        std::string r = reg();
        TokenKind op = e->op;

        auto cmp_result = [&](const std::string& c) -> EV {
            const char* pred = nullptr;
            switch (op) {
                case TokenKind::eq: pred = "eq"; break;
                case TokenKind::neq: pred = "ne"; break;
                case TokenKind::lt: pred = "slt"; break;
                case TokenKind::le: pred = "sle"; break;
                case TokenKind::gt: pred = "sgt"; break;
                case TokenKind::ge: pred = "sge"; break;
                default: return {"0", cg.t_bool()};
            }
            line(r + " = icmp " + pred + " i32 " + c + ", 0");
            return {r, cg.t_bool()};
        };

        if (l.second->k == Ty::str_) {
            std::string c = reg();
            line(c + " = call i32 @beans_str_cmp(ptr " + l.first + ", ptr " + r2.first + ")");
            return cmp_result(c);
        }
        if (l.second->k == Ty::dec_) {
            switch (op) {
                case TokenKind::plus:
                case TokenKind::minus:
                case TokenKind::star:
                case TokenKind::slash: {
                    const char* fn = op == TokenKind::plus    ? "add"
                                     : op == TokenKind::minus ? "sub"
                                     : op == TokenKind::star  ? "mul"
                                                              : "div";
                    line(r + " = call ptr @beans_dec_" + fn + "(ptr " + l.first +
                         ", ptr " + r2.first + ")");
                    return {r, cg.t_dec()};
                }
                default: {
                    std::string c = reg();
                    line(c + " = call i32 @beans_dec_cmp(ptr " + l.first + ", ptr " +
                         r2.first + ")");
                    return cmp_result(c);
                }
            }
        }
        if (l.second->k == Ty::enum_) {
            // payload-free enum equality: compare tags
            std::string ta = load_at(l.first, 0, cg.t_i64());
            std::string tb = load_at(r2.first, 0, cg.t_i64());
            line(r + " = icmp " + (op == TokenKind::eq ? "eq" : "ne") + " i64 " + ta +
                 ", " + tb);
            return {r, cg.t_bool()};
        }

        bool flt = l.second->k == Ty::f64_;
        auto arith = [&](const char* iop, const char* fop) -> EV {
            line(r + " = " + (flt ? fop : iop) + " " + (flt ? "double" : "i64") + " " +
                 l.first + ", " + r2.first);
            return {r, flt ? cg.t_f64() : cg.t_i64()};
        };
        auto compare = [&](const char* ipred, const char* fpred) -> EV {
            if (flt) line(r + " = fcmp " + std::string(fpred) + " double " + l.first + ", " + r2.first);
            else if (l.second->k == Ty::i1_)
                line(r + " = icmp " + std::string(ipred) + " i1 " + l.first + ", " + r2.first);
            else line(r + " = icmp " + std::string(ipred) + " i64 " + l.first + ", " + r2.first);
            return {r, cg.t_bool()};
        };
        switch (op) {
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
                return {"0", cg.t_i64()};
        }
    }

    EV eval_field(const Expr* e) {
        const Expr* obj = e->object.get();
        if (obj->kind == Expr::Kind::ident && !find_var(std::string(obj->text))) {
            std::string n(obj->text);
            // bare enum variant value: Payment.cash
            if (cg.enum_decls.count(n)) {
                int tag = cg.variant_tag(n, e->name);
                std::string b = box_enum(tag, {});
                return {b, cg.t_enum(n, {})};
            }
        }
        EV v = eval(obj);
        if (v.second->k == Ty::obj_) {
            if (v.second->name == "Error") {
                int off = e->name == "msg" ? 16 : 24;
                return {load_at(v.first, off, cg.t_str()), cg.t_str()};
            }
            auto it = cg.impl_by_name.find(v.second->name);
            if (it != cg.impl_by_name.end()) {
                for (const CImpl::FieldInfo& f : it->second->fields) {
                    if (f.name == e->name) {
                        return {load_at(v.first, f.offset, f.ty), f.ty};
                    }
                }
            }
        }
        err(e, "field '" + e->name + "'");
        return {"0", cg.t_i64()};
    }

    // where a field lives, for assignment
    std::pair<std::string, Ty*> field_ptr(const Expr* e) {
        EV v = eval(e->object.get());
        if (v.second->k == Ty::obj_) {
            auto it = cg.impl_by_name.find(v.second->name);
            if (it != cg.impl_by_name.end()) {
                for (const CImpl::FieldInfo& f : it->second->fields) {
                    if (f.name == e->name) {
                        std::string p = reg();
                        line(p + " = getelementptr i8, ptr " + v.first + ", i64 " +
                             std::to_string(f.offset));
                        return {p, f.ty};
                    }
                }
            }
        }
        err(e, "assigning to this field");
        return {"null", cg.t_i64()};
    }

    EV eval_cast(const Expr* e) {
        EV v = eval(e->object.get());
        Ty* to = rt(e->type.get(), e->line, e->col);
        if (e->checked) {
            // as? — runtime class check via the parents table
            if (v.second->k != Ty::obj_ || to->k != Ty::obj_) {
                err(e, "this as?");
                return {"null", cg.t_bad()};
            }
            auto it = cg.impl_by_name.find(to->name);
            if (it == cg.impl_by_name.end()) {
                err(e, "as? to an interface");
                return {"null", cg.t_bad()};
            }
            std::string id = load_at(v.first, 8, cg.t_i64());
            std::string c = reg(), cb = reg();
            line(c + " = call i64 @beans_is_a(i64 " + id + ", i64 " +
                 std::to_string(it->second->id) + ")");
            line(cb + " = icmp ne i64 " + c + ", 0");
            std::string yes = bb(), no = bb(), end = bb();
            std::string slot = fresh_slot("asq", cg.t_str()); // ptr slot
            line("br i1 " + cb + ", label %" + yes + ", label %" + no);
            label(yes);
            std::string sb = box_enum(0, {{v.first, to}});
            line("store ptr " + sb + ", ptr " + slot);
            line("br label %" + end);
            label(no);
            std::string nb = box_enum(1, {});
            line("store ptr " + nb + ", ptr " + slot);
            line("br label %" + end);
            label(end);
            std::string r = reg();
            line(r + " = load ptr, ptr " + slot);
            return {r, cg.t_option(to)};
        }
        if (v.second == to) return v;
        std::string r = reg();
        if (v.second->k == Ty::i64_ && to->k == Ty::f64_) {
            line(r + " = sitofp i64 " + v.first + " to double");
            return {r, to};
        }
        if (v.second->k == Ty::f64_ && to->k == Ty::i64_) {
            line(r + " = fptosi double " + v.first + " to i64");
            return {r, to};
        }
        if (v.second->k == Ty::i64_ && to->k == Ty::dec_) {
            line(r + " = call ptr @beans_dec_from_int(i64 " + v.first + ")");
            return {r, to};
        }
        if (v.second->k == Ty::dec_ && to->k == Ty::i64_) {
            line(r + " = call i64 @beans_dec_to_int(ptr " + v.first + ")");
            return {r, to};
        }
        if (v.second->k == Ty::f64_ && to->k == Ty::dec_) {
            line(r + " = call ptr @beans_dec_from_f64(double " + v.first + ")");
            return {r, to};
        }
        if (v.second->k == Ty::dec_ && to->k == Ty::f64_) {
            line(r + " = call double @beans_dec_to_f64(ptr " + v.first + ")");
            return {r, to};
        }
        if (v.second->k == Ty::obj_ && to->k == Ty::obj_) return {v.first, to}; // upcast
        if (v.second->k == Ty::i64_ && to->k == Ty::i64_) return v; // sized ints folded
        err(e, "this cast");
        return {v.first, to};
    }

    EV eval_init(const Expr* e, Ty* hint) {
        CImpl* im = nullptr;
        if (!e->name.empty()) {
            auto cit = cg.class_decls.find(e->name);
            if (cit != cg.class_decls.end()) {
                std::vector<Ty*> targs;
                for (const TypePtr& t : e->type_args)
                    targs.push_back(rt(t.get(), e->line, e->col));
                im = cg.request_impl(cit->second, std::move(targs), e->line, e->col);
            }
        } else if (hint && hint->k == Ty::obj_) {
            auto it = cg.impl_by_name.find(hint->name);
            if (it != cg.impl_by_name.end()) im = it->second;
        }
        if (!im) {
            err(e, "building this");
            return {"null", cg.t_bad()};
        }
        int size = 16 + 8 * static_cast<int>(im->fields.size());
        std::string o = alloc_bytes(size);
        line("store ptr @vt_" + im->mangled + ", ptr " + o);
        store_at(o, 8, std::to_string(im->id), cg.t_i64());
        for (const CImpl::FieldInfo& f : im->fields) {
            const InitEntry* given = nullptr;
            for (const InitEntry& en : e->entries) {
                if (en.name == f.name) given = &en;
            }
            if (given) {
                EV v = eval(given->value.get(), f.ty);
                store_at(o, f.offset, v.first, f.ty);
            } else if (f.decl->def) {
                EV v = eval(f.decl->def.get(), f.ty);
                store_at(o, f.offset, v.first, f.ty);
            } else if (f.ty->k == Ty::f64_) {
                store_at(o, f.offset, fmt_double(0), f.ty);
            } else if (f.ty->k == Ty::i64_ || f.ty->k == Ty::i1_) {
                store_at(o, f.offset, "0", f.ty);
            } else {
                store_at(o, f.offset, "null", f.ty);
            }
        }
        return {o, cg.t_obj(im->mangled)};
    }

    // ---- calls ----
    std::vector<EV> eval_args(const Expr* e, const std::vector<Param>& params,
                              const std::map<std::string, Ty*>& penv) {
        std::vector<EV> out;
        for (size_t i = 0; i < e->args.size(); i++) {
            Ty* h = i < params.size()
                        ? cg.resolve(params[i].type.get(), penv, e->line, e->col)
                        : nullptr;
            out.push_back(eval(e->args[i].get(), h));
        }
        return out;
    }
    std::string args_text(const std::vector<EV>& args, const std::string& self_val) {
        std::string s;
        bool first = true;
        if (!self_val.empty()) {
            s += "ptr " + self_val;
            first = false;
        }
        for (const EV& a : args) {
            if (!first) s += ", ";
            first = false;
            s += std::string(ll(a.second)) + " " + a.first;
        }
        return s;
    }
    EV emit_call(const std::string& target, Ty* ret, const std::string& args) {
        if (ret->k == Ty::unit_) {
            line("call void " + target + "(" + args + ")");
            return {"", ret};
        }
        std::string r = reg();
        line(r + " = call " + std::string(ll(ret)) + " " + target + "(" + args + ")");
        return {r, ret};
    }

    EV eval_call(const Expr* e, Ty* hint) {
        const Expr* callee = e->callee.get();

        if (callee->kind == Expr::Kind::ident) {
            std::string name(callee->text);
            if (name == "some") {
                Ty* inner = hint && hint->k == Ty::enum_ && !hint->args.empty()
                                ? hint->args[0]
                                : nullptr;
                EV v = eval(e->args[0].get(), inner);
                std::string b = box_enum(0, {v});
                return {b, cg.t_option(inner ? inner : v.second)};
            }
            if (name == "ok") {
                Ty* inner = hint && hint->k == Ty::enum_ && !hint->args.empty()
                                ? hint->args[0]
                                : nullptr;
                EV v = eval(e->args[0].get(), inner);
                std::string b = box_enum(0, {v});
                return {b, cg.t_result(inner ? inner : v.second,
                                       hint && hint->args.size() >= 2 ? hint->args[1]
                                                                      : cg.t_error())};
            }
            if (name == "err") {
                EV v = eval(e->args[0].get());
                std::string payload = v.first;
                Ty* pty = v.second;
                if (v.second->k == Ty::str_) {
                    payload = make_error(v.first);
                    pty = cg.t_error();
                }
                std::string b = box_enum(1, {{payload, pty}});
                Ty* ok = hint && hint->k == Ty::enum_ && !hint->args.empty()
                             ? hint->args[0]
                             : cg.t_i64();
                return {b, cg.t_result(ok, pty)};
            }
            auto fit = cg.fn_decls.find(name);
            if (fit != cg.fn_decls.end()) {
                const FnDecl* f = fit->second;
                if (!f->generics.empty()) {
                    err(e, "calling the generic function '" + name + "'");
                    return {"0", cg.t_i64()};
                }
                std::vector<EV> args = eval_args(e, f->params, CG2::empty_env());
                Ty* ret = cg.resolve(f->ret.get(), CG2::empty_env(), e->line, e->col);
                return emit_call("@b_" + name, ret, args_text(args, ""));
            }
            err(e, "calling '" + name + "'");
            return {"0", cg.t_i64()};
        }

        if (callee->kind != Expr::Kind::field) {
            err(e, "this call");
            return {"0", cg.t_i64()};
        }
        const Expr* obj = callee->object.get();
        const std::string& mname = callee->name;

        if (obj->kind == Expr::Kind::ident && !find_var(std::string(obj->text))) {
            std::string n(obj->text);
            if (n == "io" && (mname == "println" || mname == "print")) {
                EV v = eval(e->args[0].get());
                std::string s = to_str(v, e->args[0].get());
                line("call void @beans_" + mname + "(ptr " + s + ")");
                return {"", cg.t_unit()};
            }
            // enum construction
            if (cg.enum_decls.count(n)) {
                const EnumVariant* var = cg.variant_decl(n, mname);
                int tag = cg.variant_tag(n, mname);
                std::vector<EV> payload;
                for (size_t i = 0; i < e->args.size(); i++) {
                    Ty* h = var && i < var->payload.size()
                                ? cg.resolve(var->payload[i].type.get(), CG2::empty_env(),
                                             e->line, e->col)
                                : nullptr;
                    payload.push_back(eval(e->args[i].get(), h));
                }
                std::string b = box_enum(tag, payload);
                return {b, cg.t_enum(n, {})};
            }
            // user class static
            auto cit = cg.class_decls.find(n);
            if (cit != cg.class_decls.end() && !cit->second->is_interface) {
                CImpl* im = cg.request_impl(cit->second, {}, e->line, e->col);
                for (const FnDecl& m : cit->second->methods) {
                    if (m.name == mname && !m.has_self) {
                        std::vector<EV> args = eval_args(e, m.params, im->env);
                        Ty* ret = cg.resolve(m.ret.get(), im->env, e->line, e->col);
                        return emit_call("@s_" + im->mangled + "_" + mname, ret,
                                         args_text(args, ""));
                    }
                }
            }
            if (n == "thread" || n == "Mutex" || n == "Channel" || n == "AtomicInt") {
                err(e, "threads");
                return {"0", cg.t_i64()};
            }
        }

        EV recv = eval(obj);
        return method_call(e, recv, mname);
    }

    std::string make_error(const std::string& msg_val) {
        std::string o = alloc_bytes(32);
        line("store ptr null, ptr " + o);
        store_at(o, 8, "-1", cg.t_i64());
        store_at(o, 16, msg_val, cg.t_str());
        store_at(o, 24, cg.intern_string(""), cg.t_str());
        return o;
    }

    EV method_call(const Expr* e, EV recv, const std::string& mname) {
        Ty* rt_ = recv.second;

        // user classes and interfaces: virtual dispatch
        if (rt_->k == Ty::obj_ && rt_->name != "Error") {
            CG2::FoundMethod fm;
            auto it = cg.impl_by_name.find(rt_->name);
            if (it != cg.impl_by_name.end()) {
                fm = cg.find_method_class(it->second, mname, false);
            } else if (rt_->iface) {
                fm = cg.find_method_iface(rt_->iface, mname);
            }
            if (!fm.decl) {
                err(e, "method '" + mname + "'");
                return {"0", cg.t_i64()};
            }
            std::vector<EV> args = eval_args(e, fm.decl->params, *fm.env);
            Ty* ret = cg.resolve(fm.decl->ret.get(), *fm.env, e->line, e->col);
            int slot = cg.selector(mname);
            std::string vt = load_at(recv.first, 0, cg.t_str()); // ptr
            std::string sp = reg(), fp = reg();
            line(sp + " = getelementptr ptr, ptr " + vt + ", i64 " + std::to_string(slot));
            line(fp + " = load ptr, ptr " + sp);
            return emit_call(fp, ret, args_text(args, recv.first));
        }

        // enum methods (direct) and Option/Result builtins
        if (rt_->k == Ty::enum_) {
            auto eit = cg.enum_decls.find(rt_->name);
            if (eit != cg.enum_decls.end()) {
                for (const FnDecl& m : eit->second->methods) {
                    if (m.name == mname) {
                        std::vector<EV> args = eval_args(e, m.params, CG2::empty_env());
                        Ty* ret = cg.resolve(m.ret.get(), CG2::empty_env(), e->line, e->col);
                        return emit_call("@em_" + rt_->name + "_" + mname, ret,
                                         args_text(args, recv.first));
                    }
                }
            }
            Ty* inner = rt_->args.empty() ? cg.t_i64() : rt_->args[0];
            if (mname == "or") {
                EV dflt = eval(e->args[0].get(), inner);
                std::string tag = load_at(recv.first, 0, cg.t_i64());
                std::string c = reg();
                line(c + " = icmp eq i64 " + tag + ", 0");
                std::string hasb = bb(), endb = bb();
                std::string slot = fresh_slot("orv", inner);
                line("store " + std::string(ll(inner)) + " " + dflt.first + ", ptr " + slot);
                line("br i1 " + c + ", label %" + hasb + ", label %" + endb);
                label(hasb);
                std::string payload = load_at(recv.first, 8, inner);
                line("store " + std::string(ll(inner)) + " " + payload + ", ptr " + slot);
                line("br label %" + endb);
                label(endb);
                std::string r = reg();
                line(r + " = load " + std::string(ll(inner)) + ", ptr " + slot);
                return {r, inner};
            }
            if (mname == "expect") {
                EV msg = eval(e->args[0].get());
                std::string tag = load_at(recv.first, 0, cg.t_i64());
                std::string c = reg();
                line(c + " = icmp eq i64 " + tag + ", 0");
                std::string okb = bb(), badb = bb();
                line("br i1 " + c + ", label %" + okb + ", label %" + badb);
                label(badb);
                line("call void @beans_panic(ptr " + msg.first + ", i64 " +
                     std::to_string(e->line) + ", i64 " + std::to_string(e->col) + ")");
                line("unreachable");
                label(okb);
                std::string payload = load_at(recv.first, 8, inner);
                return {payload, inner};
            }
            if (mname == "is_some" || mname == "is_ok" || mname == "is_none") {
                std::string tag = load_at(recv.first, 0, cg.t_i64());
                std::string r = reg();
                line(r + " = icmp " + (mname == "is_none" ? "ne" : "eq") + " i64 " + tag +
                     ", 0");
                return {r, cg.t_bool()};
            }
            err(e, "method '" + mname + "'");
            return {"0", cg.t_i64()};
        }

        // builtins on primitives / string / list / decimal
        switch (rt_->k) {
            case Ty::i64_:
                if (mname == "abs") {
                    std::string neg = reg(), c = reg(), r = reg();
                    line(neg + " = sub i64 0, " + recv.first);
                    line(c + " = icmp slt i64 " + recv.first + ", 0");
                    line(r + " = select i1 " + c + ", i64 " + neg + ", i64 " + recv.first);
                    return {r, cg.t_i64()};
                }
                break;
            case Ty::f64_:
                if (mname == "abs") {
                    std::string r = reg();
                    line(r + " = call double @llvm.fabs.f64(double " + recv.first + ")");
                    return {r, cg.t_f64()};
                }
                if (mname == "round") {
                    std::string r = reg();
                    line(r + " = call i64 @beans_f64_round(double " + recv.first + ")");
                    return {r, cg.t_i64()};
                }
                break;
            case Ty::dec_:
                if (mname == "abs") {
                    std::string r = reg();
                    line(r + " = call ptr @beans_dec_abs(ptr " + recv.first + ")");
                    return {r, cg.t_dec()};
                }
                if (mname == "round") {
                    EV p = eval(e->args[0].get());
                    std::string r = reg();
                    line(r + " = call ptr @beans_dec_round(ptr " + recv.first + ", i64 " +
                         p.first + ")");
                    return {r, cg.t_dec()};
                }
                break;
            case Ty::str_: {
                if (mname == "len") {
                    std::string r = reg();
                    line(r + " = call i64 @beans_str_len(ptr " + recv.first + ")");
                    return {r, cg.t_i64()};
                }
                if (mname == "last") {
                    EV n = eval(e->args[0].get());
                    std::string r = reg();
                    line(r + " = call ptr @beans_str_last(ptr " + recv.first + ", i64 " +
                         n.first + ")");
                    return {r, cg.t_str()};
                }
                if (mname == "contains") {
                    EV n = eval(e->args[0].get());
                    std::string c = reg(), r = reg();
                    line(c + " = call i64 @beans_str_contains(ptr " + recv.first +
                         ", ptr " + n.first + ")");
                    line(r + " = icmp ne i64 " + c + ", 0");
                    return {r, cg.t_bool()};
                }
                if (mname == "to_int") {
                    std::string okp = fresh_slot("okf", cg.t_i64());
                    std::string v = reg();
                    line(v + " = call i64 @beans_parse_int(ptr " + recv.first + ", ptr " +
                         okp + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okp);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string okb = bb(), errb = bb(), endb = bb();
                    std::string slot = fresh_slot("res", cg.t_str());
                    line("br i1 " + c + ", label %" + okb + ", label %" + errb);
                    label(okb);
                    std::string ob = box_enum(0, {{v, cg.t_i64()}});
                    line("store ptr " + ob + ", ptr " + slot);
                    line("br label %" + endb);
                    label(errb);
                    // message matches the interpreter exactly
                    std::string m1 = reg(), m2 = reg();
                    line(m1 + " = call ptr @beans_concat(ptr " +
                         cg.intern_string("can't read '") + ", ptr " + recv.first + ")");
                    line(m2 + " = call ptr @beans_concat(ptr " + m1 + ", ptr " +
                         cg.intern_string("' as int") + ")");
                    std::string eo = make_error(m2);
                    std::string eb = box_enum(1, {{eo, cg.t_error()}});
                    line("store ptr " + eb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    return {r, cg.t_result(cg.t_i64(), cg.t_error())};
                }
                break;
            }
            case Ty::list_: {
                Ty* elem = rt_->args[0];
                if (mname == "push") {
                    EV v = eval(e->args[0].get(), elem);
                    line("call void @beans_list_push(ptr " + recv.first + ", i64 " +
                         to_slot(v) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "len") {
                    return {load_at(recv.first, 8, cg.t_i64()), cg.t_i64()};
                }
                if (mname == "pop") {
                    std::string len = load_at(recv.first, 8, cg.t_i64());
                    std::string c = reg();
                    line(c + " = icmp sgt i64 " + len + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("pop", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string n1 = reg();
                    line(n1 + " = sub i64 " + len + ", 1");
                    store_at(recv.first, 8, n1, cg.t_i64());
                    std::string data = load_at(recv.first, 0, cg.t_str());
                    std::string ep = reg(), raw = reg();
                    line(ep + " = getelementptr i64, ptr " + data + ", i64 " + n1);
                    line(raw + " = load i64, ptr " + ep);
                    std::string sb = box_enum(0, {{from_slot(raw, elem), elem}});
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = box_enum(1, {});
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    return {r, cg.t_option(elem)};
                }
                if (mname == "get") {
                    EV idx = eval(e->args[0].get());
                    std::string len = load_at(recv.first, 8, cg.t_i64());
                    std::string c = reg();
                    line(c + " = icmp ult i64 " + idx.first + ", " + len);
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("get", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string data = load_at(recv.first, 0, cg.t_str());
                    std::string ep = reg(), raw = reg();
                    line(ep + " = getelementptr i64, ptr " + data + ", i64 " + idx.first);
                    line(raw + " = load i64, ptr " + ep);
                    std::string sb = box_enum(0, {{from_slot(raw, elem), elem}});
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = box_enum(1, {});
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    return {r, cg.t_option(elem)};
                }
                break;
            }
            default:
                break;
        }
        err(e, "method '" + mname + "'");
        return {"0", cg.t_i64()};
    }

    // ---- match ----
    EV eval_match(const Expr* e, Ty* hint) {
        EV subj = eval(e->subject.get());
        std::string endb = bb();
        Ty* result = hint;
        bool unit_result = false;
        std::string slot;
        if (result && result->k != Ty::unit_) slot = fresh_slot("mat", result);

        for (size_t ai = 0; ai < e->arms.size(); ai++) {
            const MatchArm& arm = e->arms[ai];
            std::string armb = bb();
            std::string nextb = ai + 1 < e->arms.size() ? bb() : endb;

            std::string cond = pattern_cond(arm.pat.get(), subj);
            line("br i1 " + cond + ", label %" + armb + ", label %" + nextb);
            label(armb);
            scopes.emplace_back();
            bind_pattern(arm.pat.get(), subj);
            EV v = eval(arm.value.get(), result && !unit_result ? result : nullptr);
            scopes.pop_back();
            if (!result && !unit_result) {
                if (v.second->k == Ty::unit_) unit_result = true;
                else {
                    result = v.second;
                    slot = fresh_slot("mat", result);
                }
            }
            if (result && !unit_result && v.second->k != Ty::unit_) {
                line("store " + std::string(ll(result)) + " " + v.first + ", ptr " + slot);
            }
            if (!terminated) line("br label %" + endb);
            if (ai + 1 < e->arms.size()) label(nextb);
        }
        label(endb);
        if (unit_result || !result) return {"", cg.t_unit()};
        std::string r = reg();
        line(r + " = load " + std::string(ll(result)) + ", ptr " + slot);
        return {r, result};
    }

    std::string pattern_cond(const Pattern* p, const EV& subj) {
        switch (p->kind) {
            case Pattern::Kind::wildcard:
                return "true";
            case Pattern::Kind::alt: {
                std::string acc;
                for (const PatPtr& a : p->alts) {
                    std::string c = pattern_cond(a.get(), subj);
                    if (acc.empty()) {
                        acc = c;
                    } else {
                        std::string r = reg();
                        line(r + " = or i1 " + acc + ", " + c);
                        acc = r;
                    }
                }
                return acc;
            }
            case Pattern::Kind::literal: {
                EV lit = eval(p->lit.get(), subj.second);
                std::string r = reg();
                if (subj.second->k == Ty::str_) {
                    std::string c = reg();
                    line(c + " = call i32 @beans_str_cmp(ptr " + subj.first + ", ptr " +
                         lit.first + ")");
                    line(r + " = icmp eq i32 " + c + ", 0");
                } else if (subj.second->k == Ty::f64_) {
                    line(r + " = fcmp oeq double " + subj.first + ", " + lit.first);
                } else if (subj.second->k == Ty::i1_) {
                    line(r + " = icmp eq i1 " + subj.first + ", " + lit.first);
                } else if (subj.second->k == Ty::dec_) {
                    std::string c = reg();
                    line(c + " = call i32 @beans_dec_cmp(ptr " + subj.first + ", ptr " +
                         lit.first + ")");
                    line(r + " = icmp eq i32 " + c + ", 0");
                } else {
                    line(r + " = icmp eq i64 " + subj.first + ", " + lit.first);
                }
                return r;
            }
            case Pattern::Kind::range: {
                EV lo = eval(p->lit.get());
                EV hi = eval(p->lit2.get());
                std::string a = reg(), b2 = reg(), r = reg();
                line(a + " = icmp sge i64 " + subj.first + ", " + lo.first);
                line(b2 + " = icmp " + (p->inclusive ? "sle" : "slt") + " i64 " +
                     subj.first + ", " + hi.first);
                line(r + " = and i1 " + a + ", " + b2);
                return r;
            }
            case Pattern::Kind::name: {
                int tag = cg.variant_tag(subj.second->name, p->name);
                std::string t = load_at(subj.first, 0, cg.t_i64());
                std::string r = reg();
                line(r + " = icmp eq i64 " + t + ", " + std::to_string(tag));
                return r;
            }
        }
        return "false";
    }

    std::vector<Ty*> payload_types(const std::string& enum_name, const std::string& variant,
                                   const std::vector<Ty*>& subj_args,
                                   uint32_t line_, uint32_t col_) {
        if (enum_name == "Option") {
            return variant == "some" && !subj_args.empty() ? std::vector<Ty*>{subj_args[0]}
                                                           : std::vector<Ty*>{};
        }
        if (enum_name == "Result") {
            if (variant == "ok" && !subj_args.empty()) return {subj_args[0]};
            if (variant == "err" && subj_args.size() >= 2) return {subj_args[1]};
            return {};
        }
        std::vector<Ty*> out;
        if (const EnumVariant* v = cg.variant_decl(enum_name, variant)) {
            for (const Param& p : v->payload) {
                out.push_back(cg.resolve(p.type.get(), CG2::empty_env(), line_, col_));
            }
        }
        return out;
    }

    void bind_pattern(const Pattern* p, const EV& subj) {
        if (p->kind != Pattern::Kind::name || p->bindings.empty()) return;
        std::vector<Ty*> ptys =
            payload_types(subj.second->name, p->name, subj.second->args, p->line, p->col);
        for (size_t i = 0; i < p->bindings.size() && i < ptys.size(); i++) {
            std::string v = load_at(subj.first, 8 + 8 * static_cast<int>(i), ptys[i]);
            std::string slot = alloc_slot(p->bindings[i].name, ptys[i]);
            line("store " + std::string(ll(ptys[i])) + " " + v + ", ptr " + slot);
        }
    }

    // ---- statements ----
    void exec_block(const std::vector<StmtPtr>& stmts) {
        scopes.emplace_back();
        for (const StmtPtr& s : stmts) {
            if (terminated) break;
            exec(s.get());
        }
        scopes.pop_back();
    }

    void exec(const Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::let_: {
                Ty* t = rt(s->type.get(), s->line, s->col);
                if (t->k == Ty::bad_ || t->k == Ty::unit_) return;
                EV v = eval(s->init.get(), t);
                std::string slot = alloc_slot(s->name, t);
                line("store " + std::string(ll(t)) + " " + v.first + ", ptr " + slot);
                break;
            }
            case Stmt::Kind::assign: {
                std::string ptr;
                Ty* t = nullptr;
                if (s->target->kind == Expr::Kind::ident) {
                    Var* var = find_var(std::string(s->target->text));
                    if (!var) { cg.err(s->line, s->col, "this assignment"); return; }
                    ptr = var->slot;
                    t = var->ty;
                } else if (s->target->kind == Expr::Kind::field) {
                    auto [p, ft] = field_ptr(s->target.get());
                    ptr = p;
                    t = ft;
                } else {
                    cg.err(s->line, s->col, "assigning here");
                    return;
                }
                EV v = eval(s->value.get(), t);
                if (s->op == TokenKind::assign) {
                    line("store " + std::string(ll(t)) + " " + v.first + ", ptr " + ptr);
                    return;
                }
                std::string cur = reg();
                line(cur + " = load " + std::string(ll(t)) + ", ptr " + ptr);
                std::string r = reg();
                if (t->k == Ty::dec_) {
                    const char* fn = s->op == TokenKind::plus_eq    ? "add"
                                     : s->op == TokenKind::minus_eq ? "sub"
                                     : s->op == TokenKind::star_eq  ? "mul"
                                                                    : "div";
                    line(r + " = call ptr @beans_dec_" + fn + "(ptr " + cur + ", ptr " +
                         v.first + ")");
                } else {
                    bool flt = t->k == Ty::f64_;
                    const char* op = nullptr;
                    switch (s->op) {
                        case TokenKind::plus_eq: op = flt ? "fadd" : "add"; break;
                        case TokenKind::minus_eq: op = flt ? "fsub" : "sub"; break;
                        case TokenKind::star_eq: op = flt ? "fmul" : "mul"; break;
                        case TokenKind::slash_eq: op = flt ? "fdiv" : "sdiv"; break;
                        case TokenKind::percent_eq: op = flt ? "frem" : "srem"; break;
                        default: return;
                    }
                    line(r + " = " + op + " " + (flt ? "double" : "i64") + " " + cur +
                         ", " + v.first);
                }
                line("store " + std::string(ll(t)) + " " + r + ", ptr " + ptr);
                break;
            }
            case Stmt::Kind::expr:
                eval(s->expr.get());
                break;
            case Stmt::Kind::ret: {
                if (is_main) {
                    line("ret i32 0");
                } else if (s->expr) {
                    EV v = eval(s->expr.get(), ret_ty);
                    line("ret " + std::string(ll(v.second)) + " " + v.first);
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
                EV c = eval(s->cond.get());
                std::string then_bb = bb();
                std::string else_bb = s->else_body.empty() ? "" : bb();
                std::string end_bb = bb();
                line("br i1 " + c.first + ", label %" + then_bb + ", label %" +
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
                EV c = eval(s->cond.get());
                line("br i1 " + c.first + ", label %" + body_bb + ", label %" + end);
                label(body_bb);
                loop_stack.push_back({end, head});
                exec_block(s->body);
                loop_stack.pop_back();
                if (!terminated) line("br label %" + head);
                label(end);
                break;
            }
            case Stmt::Kind::for_in:
                exec_for_in(s);
                break;
            case Stmt::Kind::unsafe_:
                exec_block(s->body);
                break;
            default:
                cg.err(s->line, s->col, "this statement");
                break;
        }
    }

    void exec_for_in(const Stmt* s) {
        if (s->iterable && s->iterable->kind == Expr::Kind::range) {
            EV lo = eval(s->iterable->lhs.get());
            EV hi = eval(s->iterable->rhs.get());
            scopes.emplace_back();
            std::string slot = alloc_slot(s->loop_var, cg.t_i64());
            line("store i64 " + lo.first + ", ptr " + slot);
            std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
            line("br label %" + head);
            label(head);
            std::string cur = reg(), c = reg();
            line(cur + " = load i64, ptr " + slot);
            line(c + " = icmp " + (s->iterable->inclusive ? "sle" : "slt") + " i64 " +
                 cur + ", " + hi.first);
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
            return;
        }
        // list iteration by index
        EV it = eval(s->iterable.get());
        if (it.second->k != Ty::list_) {
            cg.err(s->line, s->col, "looping over this");
            return;
        }
        Ty* elem = it.second->args[0];
        scopes.emplace_back();
        std::string idx = fresh_slot("idx", cg.t_i64());
        line("store i64 0, ptr " + idx);
        std::string var_slot = alloc_slot(s->loop_var, elem);
        std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
        line("br label %" + head);
        label(head);
        std::string i = reg(), len = load_at(it.first, 8, cg.t_i64()), c = reg();
        line(i + " = load i64, ptr " + idx);
        line(c + " = icmp slt i64 " + i + ", " + len);
        line("br i1 " + c + ", label %" + body_bb + ", label %" + end);
        label(body_bb);
        std::string data = load_at(it.first, 0, cg.t_str());
        std::string ep = reg(), raw = reg();
        line(ep + " = getelementptr i64, ptr " + data + ", i64 " + i);
        line(raw + " = load i64, ptr " + ep);
        line("store " + std::string(ll(elem)) + " " + from_slot(raw, elem) + ", ptr " +
             var_slot);
        loop_stack.push_back({end, step});
        exec_block(s->body);
        loop_stack.pop_back();
        if (!terminated) line("br label %" + step);
        label(step);
        std::string i2 = reg(), n2 = reg();
        line(i2 + " = load i64, ptr " + idx);
        line(n2 + " = add i64 " + i2 + ", 1");
        line("store i64 " + n2 + ", ptr " + idx);
        line("br label %" + head);
        label(end);
        scopes.pop_back();
    }

    // ---- whole function ----
    std::string emit() {
        std::map<std::string, Ty*> use_env = env;
        ret_ty = cg.resolve(decl.ret.get(), use_env, decl.line, decl.col);

        Ty* self_ty = nullptr;
        if (decl.has_self) {
            if (self_impl) self_ty = cg.t_obj(self_impl->mangled);
            else if (self_iface) self_ty = cg.t_obj(self_iface->name, self_iface);
            else if (self_enum) self_ty = cg.t_enum(self_enum->name, {});
        }

        std::string header = "define ";
        header += is_main ? "i32" : ll(ret_ty);
        header += " " + symbol + "(";
        scopes.emplace_back();

        std::string param_stores;
        bool first = true;
        if (self_ty) {
            header += "ptr %self.arg";
            first = false;
            std::string slot = alloc_slot("self", self_ty);
            param_stores += "  store ptr %self.arg, ptr " + slot + "\n";
        }
        for (size_t i = 0; i < decl.params.size(); i++) {
            Ty* pt = cg.resolve(decl.params[i].type.get(), use_env,
                                decl.params[i].line, decl.params[i].col);
            if (!first) header += ", ";
            first = false;
            std::string preg = "%p" + std::to_string(i);
            header += std::string(ll(pt)) + " " + preg;
            std::string slot = alloc_slot(decl.params[i].name, pt);
            param_stores += "  store " + std::string(ll(pt)) + " " + preg + ", ptr " +
                            slot + "\n";
        }
        header += ") {\nentry:\n";

        std::string firstb = bb();
        exec_block(decl.body);
        if (!terminated) {
            if (is_main) line("ret i32 0");
            else if (ret_ty->k == Ty::unit_) line("ret void");
            else if (ret_ty->k == Ty::f64_) line("ret double " + fmt_double(0));
            else if (ret_ty->k == Ty::i64_ || ret_ty->k == Ty::i1_)
                line("ret " + std::string(ll(ret_ty)) + " 0");
            else line("ret ptr null");
        }
        scopes.pop_back();

        return header + allocas + param_stores + "  br label %" + firstb + "\n" +
               firstb + ":\n" + body + "}\n\n";
    }
};

// ---- CodeGen driver ---------------------------------------------------------

CodeGen::CodeGen(const Module& mod) : mod_(mod) {}

void CodeGen::error_at(uint32_t line, uint32_t col, std::string msg) {
    errors_.push_back({std::move(msg), line, col});
}

std::string CodeGen::generate() {
    CG2 cg(mod_, errors_);

    // top-level functions
    for (const FnDecl& f : mod_.fns) {
        if (!f.generics.empty()) {
            cg.err(f.line, f.col, "generic functions");
            continue;
        }
        FnEmit fe(cg, f, f.name == "main" ? "@main" : "@b_" + f.name, f.name == "main",
                  nullptr, nullptr, nullptr, CG2::empty_env());
        cg.fn_text += fe.emit();
    }

    // enum methods
    for (const EnumDecl& e : mod_.enums) {
        for (const FnDecl& m : e.methods) {
            FnEmit fe(cg, m, "@em_" + e.name + "_" + m.name, false, nullptr, nullptr, &e,
                      CG2::empty_env());
            cg.fn_text += fe.emit();
        }
    }

    // interface default methods (emitted once per interface)
    for (const ClassDecl& c : mod_.classes) {
        if (!c.is_interface) continue;
        for (const FnDecl& m : c.methods) {
            if (!m.has_body || !m.has_self) continue;
            FnEmit fe(cg, m, "@m_" + c.name + "_" + m.name, false, nullptr, &c, nullptr,
                      CG2::empty_env());
            cg.fn_text += fe.emit();
        }
    }

    // class methods, per instantiation (queue grows while we emit)
    std::set<std::string> emitted;
    while (true) {
        CImpl* im = nullptr;
        for (CImpl* q : cg.impl_queue) {
            if (!emitted.count(q->mangled)) { im = q; break; }
        }
        if (!im) break;
        emitted.insert(im->mangled);
        for (const FnDecl& m : im->decl->methods) {
            if (!m.has_body) continue;
            std::string sym = (m.has_self ? "@m_" : "@s_") + im->mangled + "_" + m.name;
            FnEmit fe(cg, m, sym, false, im, nullptr, nullptr, im->env);
            cg.fn_text += fe.emit();
        }
    }

    if (!errors_.empty()) return "";

    // vtables: every impl gets a table over the global selector set
    int nsel = static_cast<int>(cg.selectors.size());
    std::string tables;
    for (const auto& up : cg.impls) {
        CImpl* im = up.get();
        tables += "@vt_" + im->mangled + " = internal constant [" +
                  std::to_string(nsel > 0 ? nsel : 1) + " x ptr] [";
        for (int s = 0; s < (nsel > 0 ? nsel : 1); s++) {
            if (s) tables += ", ";
            std::string sym = "null";
            for (const auto& [name, idx] : cg.selectors) {
                if (idx != s) continue;
                CG2::FoundMethod fm = cg.find_method_class(im, name, true);
                if (fm.decl) sym = "@m_" + fm.owner + "_" + name;
            }
            tables += "ptr " + sym;
        }
        tables += "]\n";
    }
    // class parent table for as?
    tables += "@beans_class_parents = global [" +
              std::to_string(cg.impls.empty() ? 1 : cg.impls.size()) + " x i64] [";
    if (cg.impls.empty()) {
        tables += "i64 -1";
    } else {
        for (size_t i = 0; i < cg.impls.size(); i++) {
            if (i) tables += ", ";
            tables += "i64 " +
                      std::to_string(cg.impls[i]->parent ? cg.impls[i]->parent->id : -1);
        }
    }
    tables += "]\n";

    std::string out;
    out += "; generated by beansc\n";
    out += "declare ptr @beans_alloc(i64)\n";
    out += "declare ptr @beans_from_int(i64)\n";
    out += "declare ptr @beans_from_float(double)\n";
    out += "declare ptr @beans_from_bool(i32)\n";
    out += "declare ptr @beans_concat(ptr, ptr)\n";
    out += "declare void @beans_println(ptr)\n";
    out += "declare void @beans_print(ptr)\n";
    out += "declare i32 @beans_str_cmp(ptr, ptr)\n";
    out += "declare i64 @beans_str_len(ptr)\n";
    out += "declare ptr @beans_str_last(ptr, i64)\n";
    out += "declare i64 @beans_str_contains(ptr, ptr)\n";
    out += "declare i64 @beans_parse_int(ptr, ptr)\n";
    out += "declare void @beans_panic(ptr, i64, i64)\n";
    out += "declare i64 @beans_is_a(i64, i64)\n";
    out += "declare ptr @beans_list_new()\n";
    out += "declare void @beans_list_push(ptr, i64)\n";
    out += "declare i64 @beans_f64_round(double)\n";
    out += "declare double @llvm.fabs.f64(double)\n";
    out += "declare ptr @beans_dec_new(i128, i64)\n";
    out += "declare ptr @beans_dec_add(ptr, ptr)\n";
    out += "declare ptr @beans_dec_sub(ptr, ptr)\n";
    out += "declare ptr @beans_dec_mul(ptr, ptr)\n";
    out += "declare ptr @beans_dec_div(ptr, ptr)\n";
    out += "declare ptr @beans_dec_neg(ptr)\n";
    out += "declare ptr @beans_dec_abs(ptr)\n";
    out += "declare ptr @beans_dec_round(ptr, i64)\n";
    out += "declare i32 @beans_dec_cmp(ptr, ptr)\n";
    out += "declare ptr @beans_dec_str(ptr)\n";
    out += "declare ptr @beans_dec_from_int(i64)\n";
    out += "declare ptr @beans_dec_from_f64(double)\n";
    out += "declare i64 @beans_dec_to_int(ptr)\n";
    out += "declare double @beans_dec_to_f64(ptr)\n\n";
    out += cg.globals;
    out += "\n";
    out += tables;
    out += "\n";
    out += cg.fn_text;
    return out;
}

const char* CodeGen::runtime_c() {
    return R"RT(// beans native runtime v2 — strings, lists, decimal, panics, class checks.
// v1 memory note: heap values are never freed (RC comes later).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* beans_alloc(long long n) { return calloc(1, (size_t)n); }

void beans_panic(const char* msg, long long line, long long col) {
    fprintf(stderr, "runtime panic at %lld:%lld: %s\n", line, col, msg);
    exit(3);
}

// ---- strings ----
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
long long beans_str_len(char* s) { return (long long)strlen(s); }
char* beans_str_last(char* s, long long n) {
    long long len = (long long)strlen(s);
    if (n < 0) n = 0;
    if (n > len) n = len;
    return strdup(s + (len - n));
}
long long beans_str_contains(char* s, char* sub) { return strstr(s, sub) != NULL; }
long long beans_parse_int(char* s, long long* ok) {
    char* end = NULL;
    long long v = strtoll(s, &end, 10);
    *ok = (end != s && *end == '\0');
    return v;
}
long long beans_f64_round(double v) { return llround(v); }

// ---- lists: { data, len, cap } of raw 8-byte slots ----
typedef struct {
    long long* data;
    long long len, cap;
} BList;
BList* beans_list_new(void) {
    BList* l = calloc(1, sizeof(BList));
    l->cap = 4;
    l->data = calloc(4, 8);
    return l;
}
void beans_list_push(BList* l, long long v) {
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, (size_t)l->cap * 8);
    }
    l->data[l->len++] = v;
}

// ---- class hierarchy (table emitted by the compiler) ----
extern long long beans_class_parents[];
long long beans_is_a(long long id, long long target) {
    while (id >= 0) {
        if (id == target) return 1;
        id = beans_class_parents[id];
    }
    return 0;
}

// ---- decimal: 128-bit coefficient + base-10 scale (same math as the interpreter) ----
typedef struct {
    __int128 c;
    long long s;
} BDec;
static BDec* dec_mk(__int128 c, long long s) {
    BDec* d = malloc(sizeof(BDec));
    d->c = c;
    d->s = s;
    return d;
}
static __int128 pow10i(long long n) {
    __int128 p = 1;
    for (long long i = 0; i < n; i++) p *= 10;
    return p;
}
BDec* beans_dec_new(__int128 c, long long s) { return dec_mk(c, s); }
BDec* beans_dec_from_int(long long v) { return dec_mk((__int128)v, 0); }
static void dec_align(BDec* a, BDec* b, __int128* ca, __int128* cb, long long* s) {
    *s = a->s > b->s ? a->s : b->s;
    *ca = a->c * pow10i(*s - a->s);
    *cb = b->c * pow10i(*s - b->s);
}
BDec* beans_dec_add(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return dec_mk(ca + cb, s);
}
BDec* beans_dec_sub(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return dec_mk(ca - cb, s);
}
BDec* beans_dec_mul(BDec* a, BDec* b) { return dec_mk(a->c * b->c, a->s + b->s); }
BDec* beans_dec_div(BDec* a, BDec* b) {
    if (b->c == 0) beans_panic("divide by zero", 0, 0);
    long long extra = 20;
    __int128 num = a->c * pow10i(extra + b->s);
    __int128 q = num / b->c;
    long long s = a->s + extra;
    while (s > 0 && q % 10 == 0) {
        q /= 10;
        s -= 1;
    }
    return dec_mk(q, s);
}
BDec* beans_dec_neg(BDec* a) { return dec_mk(-a->c, a->s); }
BDec* beans_dec_abs(BDec* a) { return dec_mk(a->c < 0 ? -a->c : a->c, a->s); }
BDec* beans_dec_round(BDec* a, long long places) {
    if (places >= a->s) return dec_mk(a->c, a->s);
    __int128 f = pow10i(a->s - places);
    __int128 q = a->c / f, rem = a->c % f;
    if (rem < 0) rem = -rem;
    if (rem * 2 >= f) q += a->c >= 0 ? 1 : -1;
    return dec_mk(q, places);
}
int beans_dec_cmp(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return ca < cb ? -1 : ca > cb ? 1 : 0;
}
long long beans_dec_to_int(BDec* a) { return (long long)(a->c / pow10i(a->s)); }
double beans_dec_to_f64(BDec* a) { return (double)a->c / (double)pow10i(a->s); }
BDec* beans_dec_from_f64(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    // parse the text form so the double's decimal digits carry over
    __int128 c = 0;
    long long s = 0;
    int neg = 0, after = 0;
    long long ex = 0;
    for (const char* p = buf; *p; p++) {
        if (*p == '-') { neg = 1; continue; }
        if (*p == '+') continue;
        if (*p == '.') { after = 1; continue; }
        if (*p == 'e' || *p == 'E') {
            ex = strtoll(p + 1, NULL, 10);
            break;
        }
        c = c * 10 + (*p - '0');
        if (after) s += 1;
    }
    s -= ex;
    if (s < 0) {
        c *= pow10i(-s);
        s = 0;
    }
    if (neg) c = -c;
    return dec_mk(c, s);
}
char* beans_dec_str(BDec* a) {
    __int128 c = a->c;
    int neg = c < 0;
    if (neg) c = -c;
    char digits[64];
    int n = 0;
    if (c == 0) digits[n++] = '0';
    while (c > 0) {
        digits[n++] = (char)('0' + (int)(c % 10));
        c /= 10;
    }
    while (n <= a->s) digits[n++] = '0';
    char out[80];
    int o = 0;
    if (neg) out[o++] = '-';
    for (int i = n - 1; i >= 0; i--) {
        out[o++] = digits[i];
        if (i == a->s && i != 0) out[o++] = '.';
    }
    out[o] = '\0';
    return strdup(out);
}
)RT";
}

} // namespace beans
