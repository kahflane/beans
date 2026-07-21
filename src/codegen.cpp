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
    enum K {
        i64_, f64_, i1_, str_, unit_, dec_, obj_, enum_, list_, bad_,
        map_,     // args = {K, V}
        fn_,      // args = params..., ret last
        thread_,  // args = {ret}
        mutex_,   // args = {inner}
        chan_,    // args = {elem}
        atomic_,
    };
    K k = K::bad_;
    std::string name;          // obj: impl/iface name, enum_: enum name
    std::vector<Ty*> args;     // enum_ args; list_ elem in args[0]
    const ClassDecl* iface = nullptr; // obj typed as an interface

    Ty* fn_ret() const { return args.empty() ? nullptr : args.back(); }
    size_t fn_nparams() const { return args.empty() ? 0 : args.size() - 1; }
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
    Ty* t_kind1(Ty::K k, Ty* a) { Ty t; t.k = k; t.args = {a}; return intern(std::move(t)); }
    Ty* t_map(Ty* k, Ty* v) { Ty t; t.k = Ty::map_; t.args = {k, v}; return intern(std::move(t)); }
    Ty* t_fn(std::vector<Ty*> params_then_ret) {
        Ty t; t.k = Ty::fn_; t.args = std::move(params_then_ret);
        return intern(std::move(t));
    }
    Ty* t_atomic() { return prim(Ty::atomic_); }

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
            case Ty::map_: return "Map_" + mangle(t->args[0]) + "_" + mangle(t->args[1]);
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
            std::vector<Ty*> sig;
            for (const TypePtr& p : t->fn_params)
                sig.push_back(resolve(p.get(), env, line, col));
            sig.push_back(t->fn_ret ? resolve(t->fn_ret.get(), env, line, col) : t_unit());
            return t_fn(std::move(sig));
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
        if (n == "Map" && t->args.size() == 2)
            return t_map(resolve(t->args[0].get(), env, line, col),
                         resolve(t->args[1].get(), env, line, col));
        if (n == "Thread" && t->args.size() == 1)
            return t_kind1(Ty::thread_, resolve(t->args[0].get(), env, line, col));
        if (n == "Mutex" && t->args.size() == 1)
            return t_kind1(Ty::mutex_, resolve(t->args[0].get(), env, line, col));
        if (n == "Channel" && t->args.size() == 1)
            return t_kind1(Ty::chan_, resolve(t->args[0].get(), env, line, col));
        if (n == "AtomicInt") return t_atomic();
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

    // lifted closure fns + monomorphized generic fns, appended at the end
    std::string lifted;
    int next_clo = 0;
    struct FnInst {
        const FnDecl* decl;
        std::map<std::string, Ty*> env;
        std::string symbol;
    };
    std::vector<FnInst> fn_queue;
    std::set<std::string> fn_emitted;

    std::string request_fn(const FnDecl* decl, std::map<std::string, Ty*> env) {
        std::string sym = "@b_" + decl->name;
        for (const GenericParam& g : decl->generics) {
            auto it = env.find(g.name);
            sym += "$" + (it != env.end() ? mangle(it->second) : "x");
        }
        for (const FnInst& fi : fn_queue) {
            if (fi.symbol == sym) return sym;
        }
        fn_queue.push_back({decl, std::move(env), sym});
        return sym;
    }

    // structural match of a declared param type against a concrete arg type,
    // binding the fn's generic parameter names
    void unify_tref(const TypeRef* p, Ty* arg, const std::set<std::string>& gens,
                    std::map<std::string, Ty*>& env) {
        if (!p || !arg) return;
        if (p->kind == TypeRef::Kind::fn) {
            if (arg->k != Ty::fn_) return;
            for (size_t i = 0; i < p->fn_params.size() && i < arg->fn_nparams(); i++) {
                unify_tref(p->fn_params[i].get(), arg->args[i], gens, env);
            }
            if (p->fn_ret) unify_tref(p->fn_ret.get(), arg->fn_ret(), gens, env);
            return;
        }
        if (gens.count(p->name)) {
            if (!env.count(p->name)) env[p->name] = arg;
            return;
        }
        if (p->name == "List" && arg->k == Ty::list_ && p->args.size() == 1) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            return;
        }
        if (p->name == "Map" && arg->k == Ty::map_ && p->args.size() == 2) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            unify_tref(p->args[1].get(), arg->args[1], gens, env);
            return;
        }
        if ((p->name == "Option" || p->name == "Result") && arg->k == Ty::enum_) {
            for (size_t i = 0; i < p->args.size() && i < arg->args.size(); i++) {
                unify_tref(p->args[i].get(), arg->args[i], gens, env);
            }
        }
    }
};

// ---- free-variable analysis (for closure capture) ----------------------------

struct FreeVars {
    std::set<std::string> bound;
    std::set<std::string> free;
    std::vector<std::unique_ptr<std::string>> srcs;

    void use(std::string_view name) {
        std::string n(name);
        if (!bound.count(n)) free.insert(n);
    }
    void block(const std::vector<StmtPtr>& b) {
        std::set<std::string> save = bound;
        for (const StmtPtr& s : b) stmt(s.get());
        bound = save;
    }
    void pat(const Pattern* p) {
        if (!p) return;
        if (p->kind == Pattern::Kind::name) {
            for (const Param& b : p->bindings) bound.insert(b.name);
        }
        for (const PatPtr& a : p->alts) pat(a.get());
    }
    void stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case Stmt::Kind::let_:
                expr(s->init.get());
                bound.insert(s->name);
                break;
            case Stmt::Kind::assign:
                expr(s->target.get());
                expr(s->value.get());
                break;
            case Stmt::Kind::expr:
            case Stmt::Kind::ret:
            case Stmt::Kind::defer_:
                expr(s->expr.get());
                break;
            case Stmt::Kind::if_:
                expr(s->cond.get());
                block(s->body);
                block(s->else_body);
                break;
            case Stmt::Kind::for_ever:
                block(s->body);
                break;
            case Stmt::Kind::for_while: {
                expr(s->cond.get());
                block(s->body);
                break;
            }
            case Stmt::Kind::for_in: {
                expr(s->iterable.get());
                std::set<std::string> save = bound;
                bound.insert(s->loop_var);
                block(s->body);
                bound = save;
                break;
            }
            case Stmt::Kind::unsafe_:
                block(s->body);
                break;
            default:
                break;
        }
    }
    void expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case Expr::Kind::ident:
                use(e->text);
                break;
            case Expr::Kind::string_lit: {
                // interpolation pieces reference variables too
                for (StrPiece& p : split_interp(e->text, srcs)) {
                    if (p.expr) expr(p.expr.get());
                }
                break;
            }
            case Expr::Kind::unary:
                expr(e->rhs.get());
                break;
            case Expr::Kind::binary:
            case Expr::Kind::range:
                expr(e->lhs.get());
                expr(e->rhs.get());
                break;
            case Expr::Kind::call:
                expr(e->callee.get());
                for (const ExprPtr& a : e->args) expr(a.get());
                break;
            case Expr::Kind::field:
                expr(e->object.get());
                break;
            case Expr::Kind::index:
                expr(e->object.get());
                expr(e->index_expr.get());
                break;
            case Expr::Kind::list_lit:
                for (const ExprPtr& a : e->args) expr(a.get());
                break;
            case Expr::Kind::init:
                for (const InitEntry& en : e->entries) {
                    expr(en.key.get());
                    expr(en.value.get());
                }
                break;
            case Expr::Kind::cast:
            case Expr::Kind::try_:
                expr(e->object.get());
                break;
            case Expr::Kind::closure: {
                std::set<std::string> save = bound;
                for (const Param& p : e->params) bound.insert(p.name);
                block(e->body);
                bound = save;
                break;
            }
            case Expr::Kind::if_expr:
                expr(e->cond.get());
                expr(e->then_e.get());
                expr(e->else_e.get());
                break;
            case Expr::Kind::match_expr: {
                expr(e->subject.get());
                for (const MatchArm& a : e->arms) {
                    std::set<std::string> save = bound;
                    pat(a.pat.get());
                    expr(a.value.get());
                    bound = save;
                }
                break;
            }
            default:
                break;
        }
    }
};

// free names of a closure node (its own params/locals excluded)
static std::set<std::string> closure_free_names(const Expr* clo) {
    FreeVars fv;
    for (const Param& p : clo->params) fv.bound.insert(p.name);
    fv.block(clo->body);
    return fv.free;
}

// every name captured by any closure anywhere in this body
struct ClosureScan {
    std::set<std::string> captured;
    void block(const std::vector<StmtPtr>& b) {
        for (const StmtPtr& s : b) stmt(s.get());
    }
    void stmt(const Stmt* s) {
        if (!s) return;
        expr(s->init.get());
        expr(s->target.get());
        expr(s->value.get());
        expr(s->expr.get());
        expr(s->cond.get());
        expr(s->iterable.get());
        block(s->body);
        block(s->else_body);
    }
    void expr(const Expr* e) {
        if (!e) return;
        if (e->kind == Expr::Kind::closure) {
            std::set<std::string> f = closure_free_names(e);
            captured.insert(f.begin(), f.end());
            // and keep scanning inside for doubly nested closures
        }
        if (e->kind == Expr::Kind::string_lit) {
            std::vector<std::unique_ptr<std::string>> srcs;
            for (StrPiece& p : split_interp(e->text, srcs)) {
                if (p.expr) expr(p.expr.get());
            }
            return;
        }
        expr(e->lhs.get());
        expr(e->rhs.get());
        expr(e->callee.get());
        for (const ExprPtr& a : e->args) expr(a.get());
        expr(e->object.get());
        expr(e->index_expr.get());
        for (const InitEntry& en : e->entries) {
            expr(en.key.get());
            expr(en.value.get());
        }
        expr(e->cond.get());
        expr(e->then_e.get());
        expr(e->else_e.get());
        expr(e->subject.get());
        for (const MatchArm& a : e->arms) expr(a.value.get());
        for (const StmtPtr& s : e->body) stmt(s.get());
    }
};

// ---- per-function emitter ----------------------------------------------------

struct FnEmit {
    CG2& cg;
    std::string symbol;
    bool is_main;
    bool has_self;
    CImpl* self_impl;               // methods of a class
    const ClassDecl* self_iface;    // default methods of an interface
    const EnumDecl* self_enum;      // methods of an enum
    const std::map<std::string, Ty*>& env;

    const std::vector<Param>& params_ref;
    const TypeRef* ret_ref;
    const std::vector<StmtPtr>& body_ref;
    uint32_t dline, dcol;
    // closures: captured variables arrive as heap cells behind an env ptr
    const std::vector<std::pair<std::string, Ty*>>* captured = nullptr;

    std::string allocas, body, entry_inits;
    int next_reg = 0, next_bb = 0;
    bool terminated = false;
    struct Var { std::string slot; Ty* ty; bool boxed = false; };
    std::vector<std::map<std::string, Var>> scopes;
    std::vector<std::pair<std::string, std::string>> loop_stack;
    std::vector<std::unique_ptr<std::string>> interp_srcs;
    Ty* ret_ty = nullptr;
    std::set<std::string> boxed_names; // captured by closures
    struct DeferRec {
        const Expr* expr;
        std::string flag; // armed?
        std::vector<std::map<std::string, Var>> scope_snap; // names visible at the site
    };
    std::vector<DeferRec> defers;

    FnEmit(CG2& cg, const FnDecl& d, std::string sym, bool main, CImpl* si,
           const ClassDecl* ifc, const EnumDecl* en, const std::map<std::string, Ty*>& e)
        : cg(cg), symbol(std::move(sym)), is_main(main), has_self(d.has_self),
          self_impl(si), self_iface(ifc), self_enum(en), env(e), params_ref(d.params),
          ret_ref(d.ret.get()), body_ref(d.body), dline(d.line), dcol(d.col) {}

    // closure form
    FnEmit(CG2& cg, const Expr* clo, std::string sym,
           const std::vector<std::pair<std::string, Ty*>>* caps,
           const std::map<std::string, Ty*>& e)
        : cg(cg), symbol(std::move(sym)), is_main(false), has_self(false),
          self_impl(nullptr), self_iface(nullptr), self_enum(nullptr), env(e),
          params_ref(clo->params), ret_ref(clo->type.get()), body_ref(clo->body),
          dline(clo->line), dcol(clo->col), captured(caps) {}

    std::string reg() { return "%t" + std::to_string(next_reg++); }
    std::string bb() { return "bb" + std::to_string(next_bb++); }
    void line(const std::string& s) { body += "  " + s + "\n"; }
    void label(const std::string& l) { body += l + ":\n"; terminated = false; }
    void err(const Expr* e, const std::string& what) {
        cg.err(e ? e->line : dline, e ? e->col : dcol, what);
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
    std::string alloc_slot(const std::string& name, Ty* t, bool entry = false) {
        bool boxed = boxed_names.count(name) > 0;
        std::string slot = "%v" + std::to_string(next_reg++) + "." + name;
        allocas += "  " + slot + " = alloca " + (boxed ? "ptr" : ll(t)) + "\n";
        if (boxed) {
            // the variable lives in a heap cell so closures share it.
            // params get their cell in the entry block; lets get a fresh cell
            // at their statement site.
            std::string cell = "%cell" + std::to_string(next_reg++);
            std::string code = "  " + cell + " = call ptr @beans_alloc(i64 8)\n" +
                               "  store ptr " + cell + ", ptr " + slot + "\n";
            if (entry) entry_inits += code;
            else body += code;
        }
        scopes.back()[name] = {slot, t, boxed};
        return slot;
    }
    std::string fresh_slot(const char* tag, Ty* t) {
        std::string slot = "%v" + std::to_string(next_reg++) + "." + tag;
        allocas += "  " + slot + " = alloca " + ll(t) + "\n";
        return slot;
    }

    // read/write a Var, transparently going through its cell when boxed
    std::string var_read(Var* v) {
        std::string r = reg();
        if (!v->boxed) {
            line(r + " = load " + std::string(ll(v->ty)) + ", ptr " + v->slot);
            return r;
        }
        std::string cell = reg();
        line(cell + " = load ptr, ptr " + v->slot);
        line(r + " = load " + std::string(ll(v->ty)) + ", ptr " + cell);
        return r;
    }
    std::string var_ptr(Var* v) {
        if (!v->boxed) return v->slot;
        std::string cell = reg();
        line(cell + " = load ptr, ptr " + v->slot);
        return cell;
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
                    return {var_read(v), v->ty};
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
                    return {var_read(v), v->ty};
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
                emit_ret("ret ptr " + v.first); // pass the err/none box up unchanged
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
            case Expr::Kind::closure:
                return eval_closure(e);
            default:
                err(e, "this expression");
                return {"0", cg.t_i64()};
        }
    }

    // lambda-lift: emit the closure as a top-level fn taking its env ptr,
    // and build a box {fnptr, cell...} at the site
    EV eval_closure(const Expr* e) {
        std::vector<std::pair<std::string, Ty*>> caps;
        for (const std::string& name : closure_free_names(e)) {
            if (Var* v = find_var(name)) caps.push_back({name, v->ty});
        }

        std::string sym = "@clo" + std::to_string(cg.next_clo++);
        FnEmit fe(cg, e, sym, &caps, env);
        cg.lifted += fe.emit();

        std::vector<Ty*> sig;
        for (const Param& p : e->params) {
            sig.push_back(rt(p.type.get(), e->line, e->col));
        }
        sig.push_back(e->type ? rt(e->type.get(), e->line, e->col) : cg.t_unit());
        Ty* fty = cg.t_fn(std::move(sig));

        std::string box = alloc_bytes(8 + 8 * static_cast<int>(caps.size()));
        store_at(box, 0, sym, cg.t_str());
        for (size_t i = 0; i < caps.size(); i++) {
            Var* v = find_var(caps[i].first);
            // v is boxed (the prepass saw this closure) — pass the cell itself
            std::string cell = reg();
            line(cell + " = load ptr, ptr " + v->slot);
            store_at(box, 8 + 8 * static_cast<int>(i), cell, cg.t_str());
        }
        return {box, fty};
    }

    // call a closure/fn value: box layout {fnptr @0, cells @8...}
    EV call_fn_value(const EV& fnv, const Expr* e) {
        Ty* fty = fnv.second;
        std::vector<EV> args;
        for (size_t i = 0; i < e->args.size(); i++) {
            Ty* h = i < fty->fn_nparams() ? fty->args[i] : nullptr;
            args.push_back(eval(e->args[i].get(), h));
        }
        Ty* ret = fty->fn_ret() ? fty->fn_ret() : cg.t_unit();
        std::string fp = load_at(fnv.first, 0, cg.t_str());
        return emit_call(fp, ret, args_text(args, fnv.first));
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

    // run armed defers (newest first), then return. the deferred expression
    // compiles against the names that were visible where the defer was written.
    void emit_ret(const std::string& ret_instr) {
        for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
            std::string flag = reg();
            line(flag + " = load i1, ptr " + it->flag);
            std::string runb = bb(), skipb = bb();
            line("br i1 " + flag + ", label %" + runb + ", label %" + skipb);
            label(runb);
            std::vector<std::map<std::string, Var>> saved = std::move(scopes);
            scopes = it->scope_snap;
            eval(it->expr);
            scopes = std::move(saved);
            line("br label %" + skipb);
            label(skipb);
        }
        line(ret_instr);
    }

    void exec_index_assign(const Stmt* s) {
        EV obj = eval(s->target->object.get());
        if (obj.second->k == Ty::list_) {
            Ty* elem = obj.second->args[0];
            EV idx = eval(s->target->index_expr.get());
            std::string len = load_at(obj.first, 8, cg.t_i64());
            std::string okc = reg();
            line(okc + " = icmp ult i64 " + idx.first + ", " + len);
            std::string okb = bb(), badb = bb();
            line("br i1 " + okc + ", label %" + okb + ", label %" + badb);
            label(badb);
            std::string msg = cg.intern_string("list index out of range");
            line("call void @beans_panic(ptr " + msg + ", i64 " +
                 std::to_string(s->line) + ", i64 " + std::to_string(s->col) + ")");
            line("unreachable");
            label(okb);
            EV v = eval(s->value.get(), elem);
            std::string data = load_at(obj.first, 0, cg.t_str());
            std::string ep = reg();
            line(ep + " = getelementptr i64, ptr " + data + ", i64 " + idx.first);
            line("store i64 " + to_slot(v) + ", ptr " + ep);
            return;
        }
        if (obj.second->k == Ty::map_) {
            Ty* K = obj.second->args[0];
            Ty* V = obj.second->args[1];
            int kind = K->k == Ty::str_ ? 1 : 0;
            EV k = eval(s->target->index_expr.get(), K);
            EV v = eval(s->value.get(), V);
            line("call void @beans_map_set(ptr " + obj.first + ", i64 " + to_slot(k) +
                 ", i64 " + to_slot(v) + ", i64 " + std::to_string(kind) + ")");
            return;
        }
        cg.err(s->line, s->col, "assigning into this");
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
        // map literal / short map init
        bool is_map_hint = hint && hint->k == Ty::map_;
        bool has_expr_keys = false;
        for (const InitEntry& en : e->entries) has_expr_keys |= en.key != nullptr;
        if (e->name.empty() && (is_map_hint || has_expr_keys)) {
            Ty* K = is_map_hint ? hint->args[0] : cg.t_str();
            Ty* V = is_map_hint ? hint->args[1] : cg.t_i64();
            int kind = K->k == Ty::str_ ? 1 : 0;
            std::string m = reg();
            line(m + " = call ptr @beans_map_new()");
            for (const InitEntry& en : e->entries) {
                EV k = en.key ? eval(en.key.get(), K)
                              : EV{cg.intern_string(en.name), cg.t_str()};
                EV v = eval(en.value.get(), V);
                line("call void @beans_map_set(ptr " + m + ", i64 " + to_slot(k) +
                     ", i64 " + to_slot(v) + ", i64 " + std::to_string(kind) + ")");
            }
            return {m, cg.t_map(K, V)};
        }

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
            if (Var* v = find_var(name)) {
                EV fnv = {var_read(v), v->ty};
                if (fnv.second->k == Ty::fn_) return call_fn_value(fnv, e);
                err(e, "calling this");
                return {"0", cg.t_i64()};
            }
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
                if (f->generics.empty()) {
                    std::vector<EV> args = eval_args(e, f->params, CG2::empty_env());
                    Ty* ret = cg.resolve(f->ret.get(), CG2::empty_env(), e->line, e->col);
                    return emit_call("@b_" + name, ret, args_text(args, ""));
                }
                // monomorphize: infer the generic params from the argument types
                std::set<std::string> gens;
                for (const GenericParam& g : f->generics) gens.insert(g.name);
                std::vector<EV> args;
                std::map<std::string, Ty*> fenv;
                for (size_t i = 0; i < e->args.size(); i++) {
                    EV a = eval(e->args[i].get());
                    if (i < f->params.size()) {
                        cg.unify_tref(f->params[i].type.get(), a.second, gens, fenv);
                    }
                    args.push_back(std::move(a));
                }
                std::string sym = cg.request_fn(f, fenv);
                Ty* ret = cg.resolve(f->ret.get(), fenv, e->line, e->col);
                return emit_call(sym, ret, args_text(args, ""));
            }
            err(e, "calling '" + name + "'");
            return {"0", cg.t_i64()};
        }

        if (callee->kind != Expr::Kind::field) {
            EV fnv = eval(callee);
            if (fnv.second->k == Ty::fn_) return call_fn_value(fnv, e);
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
            if (n == "thread" && mname == "spawn") {
                EV clo = eval(e->args[0].get());
                Ty* ret = clo.second->k == Ty::fn_ && clo.second->fn_ret()
                              ? clo.second->fn_ret()
                              : cg.t_unit();
                // per-site thunk: (ptr env) -> i64, so the C runtime can call it
                std::string thunk = "@spawn_thunk" + std::to_string(cg.next_clo++);
                std::string t;
                t += "define i64 " + thunk + "(ptr %env) {\n";
                t += "  %f = load ptr, ptr %env\n";
                if (ret->k == Ty::unit_) {
                    t += "  call void %f(ptr %env)\n  ret i64 0\n}\n\n";
                } else if (ret->k == Ty::f64_) {
                    t += "  %r = call double %f(ptr %env)\n";
                    t += "  %b = bitcast double %r to i64\n  ret i64 %b\n}\n\n";
                } else if (ret->k == Ty::i64_) {
                    t += "  %r = call i64 %f(ptr %env)\n  ret i64 %r\n}\n\n";
                } else if (ret->k == Ty::i1_) {
                    t += "  %r = call i1 %f(ptr %env)\n";
                    t += "  %z = zext i1 %r to i64\n  ret i64 %z\n}\n\n";
                } else {
                    t += "  %r = call ptr %f(ptr %env)\n";
                    t += "  %z = ptrtoint ptr %r to i64\n  ret i64 %z\n}\n\n";
                }
                cg.lifted += t;
                std::string r = reg();
                line(r + " = call ptr @beans_thread_spawn(ptr " + thunk + ", ptr " +
                     clo.first + ")");
                return {r, cg.t_kind1(Ty::thread_, ret)};
            }
            if (n == "Mutex" && mname == "new") {
                Ty* inner = hint && hint->k == Ty::mutex_ ? hint->args[0] : nullptr;
                EV v = eval(e->args[0].get(), inner);
                std::string r = reg();
                line(r + " = call ptr @beans_mutex_new(i64 " + to_slot(v) + ")");
                return {r, cg.t_kind1(Ty::mutex_, inner ? inner : v.second)};
            }
            if (n == "Channel" && mname == "new") {
                EV cap = eval(e->args[0].get());
                Ty* elem = hint && hint->k == Ty::chan_ ? hint->args[0] : cg.t_i64();
                std::string r = reg();
                line(r + " = call ptr @beans_chan_new(i64 " + cap.first + ")");
                return {r, cg.t_kind1(Ty::chan_, elem)};
            }
            if (n == "AtomicInt" && mname == "new") {
                EV v = eval(e->args[0].get());
                std::string r = reg();
                line(r + " = call ptr @beans_atomic_new(i64 " + v.first + ")");
                return {r, cg.t_atomic()};
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
                // maybe a closure stored in a field
                if (it != cg.impl_by_name.end()) {
                    for (const CImpl::FieldInfo& f : it->second->fields) {
                        if (f.name == mname && f.ty->k == Ty::fn_) {
                            EV fnv = {load_at(recv.first, f.offset, f.ty), f.ty};
                            return call_fn_value(fnv, e);
                        }
                    }
                }
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
                if (mname == "max") {
                    int kind = elem->k == Ty::f64_    ? 1
                               : elem->k == Ty::str_  ? 2
                               : elem->k == Ty::dec_  ? 3
                                                      : 0;
                    std::string okf = fresh_slot("mokf", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_list_max(ptr " + recv.first +
                         ", i64 " + std::to_string(kind) + ", ptr " + okf + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("max", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
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
                if (mname == "contains") {
                    EV v = eval(e->args[0].get(), elem);
                    int kind = elem->k == Ty::str_ ? 2 : elem->k == Ty::dec_ ? 3
                               : elem->k == Ty::f64_ ? 1 : 0;
                    std::string c = reg(), r = reg();
                    line(c + " = call i64 @beans_list_contains(ptr " + recv.first +
                         ", i64 " + to_slot(v) + ", i64 " + std::to_string(kind) + ")");
                    line(r + " = icmp ne i64 " + c + ", 0");
                    return {r, cg.t_bool()};
                }
                break;
            }
            case Ty::map_: {
                Ty* K = rt_->args[0];
                Ty* V = rt_->args[1];
                int kind = K->k == Ty::str_ ? 1 : 0;
                if (mname == "set") {
                    EV k = eval(e->args[0].get(), K);
                    EV v = eval(e->args[1].get(), V);
                    line("call void @beans_map_set(ptr " + recv.first + ", i64 " +
                         to_slot(k) + ", i64 " + to_slot(v) + ", i64 " +
                         std::to_string(kind) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "get") {
                    EV k = eval(e->args[0].get(), K);
                    std::string okf = fresh_slot("gokf", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_map_get(ptr " + recv.first + ", i64 " +
                         to_slot(k) + ", i64 " + std::to_string(kind) + ", ptr " + okf + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("mg", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string sb = box_enum(0, {{from_slot(raw, V), V}});
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = box_enum(1, {});
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    return {r, cg.t_option(V)};
                }
                if (mname == "len") {
                    return {load_at(recv.first, 8, cg.t_i64()), cg.t_i64()};
                }
                if (mname == "contains") {
                    EV k = eval(e->args[0].get(), K);
                    std::string okf = fresh_slot("cokf", cg.t_i64());
                    std::string raw = reg();
                    (void)raw;
                    std::string d = reg();
                    line(d + " = call i64 @beans_map_get(ptr " + recv.first + ", i64 " +
                         to_slot(k) + ", i64 " + std::to_string(kind) + ", ptr " + okf + ")");
                    std::string okv = reg(), r = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(r + " = icmp ne i64 " + okv + ", 0");
                    return {r, cg.t_bool()};
                }
                break;
            }
            case Ty::thread_: {
                if (mname == "join") {
                    Ty* ret = rt_->args[0];
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_thread_join(ptr " + recv.first + ")");
                    if (ret->k == Ty::unit_) return {"", cg.t_unit()};
                    return {from_slot(raw, ret), ret};
                }
                break;
            }
            case Ty::mutex_: {
                if (mname == "with") {
                    Ty* inner = rt_->args[0];
                    EV clo = eval(e->args[0].get());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_mutex_lock(ptr " + recv.first + ")");
                    std::string iv = from_slot(raw, inner);
                    std::string fp = load_at(clo.first, 0, cg.t_str());
                    line("call void " + fp + "(ptr " + clo.first + ", " +
                         std::string(ll(inner)) + " " + iv + ")");
                    line("call void @beans_mutex_unlock(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::chan_: {
                Ty* elem = rt_->args[0];
                if (mname == "send") {
                    EV v = eval(e->args[0].get(), elem);
                    std::string ok = reg();
                    line(ok + " = call i64 @beans_chan_send(ptr " + recv.first + ", i64 " +
                         to_slot(v) + ")");
                    std::string c = reg();
                    line(c + " = icmp ne i64 " + ok + ", 0");
                    std::string okb = bb(), badb = bb();
                    line("br i1 " + c + ", label %" + okb + ", label %" + badb);
                    label(badb);
                    std::string msg = cg.intern_string("send on a closed channel");
                    line("call void @beans_panic(ptr " + msg + ", i64 " +
                         std::to_string(e->line) + ", i64 " + std::to_string(e->col) + ")");
                    line("unreachable");
                    label(okb);
                    return {"", cg.t_unit()};
                }
                if (mname == "recv") {
                    std::string okf = fresh_slot("rokf", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_chan_recv(ptr " + recv.first + ", ptr " +
                         okf + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("rv", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
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
                if (mname == "close") {
                    line("call void @beans_chan_close(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::atomic_: {
                if (mname == "add") {
                    EV v = eval(e->args[0].get());
                    std::string r = reg();
                    line(r + " = call i64 @beans_atomic_add(ptr " + recv.first + ", i64 " +
                         v.first + ")");
                    return {r, cg.t_i64()};
                }
                if (mname == "get") {
                    std::string r = reg();
                    line(r + " = call i64 @beans_atomic_get(ptr " + recv.first + ")");
                    return {r, cg.t_i64()};
                }
                if (mname == "set") {
                    EV v = eval(e->args[0].get());
                    line("call void @beans_atomic_set(ptr " + recv.first + ", i64 " +
                         v.first + ")");
                    return {"", cg.t_unit()};
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
                alloc_slot(s->name, t);
                Var* var = find_var(s->name);
                line("store " + std::string(ll(t)) + " " + v.first + ", ptr " +
                     var_ptr(var));
                break;
            }
            case Stmt::Kind::assign: {
                std::string ptr;
                Ty* t = nullptr;
                if (s->target->kind == Expr::Kind::ident) {
                    Var* var = find_var(std::string(s->target->text));
                    if (!var) { cg.err(s->line, s->col, "this assignment"); return; }
                    ptr = var_ptr(var);
                    t = var->ty;
                } else if (s->target->kind == Expr::Kind::field) {
                    auto [p, ft] = field_ptr(s->target.get());
                    ptr = p;
                    t = ft;
                } else if (s->target->kind == Expr::Kind::index) {
                    exec_index_assign(s);
                    return;
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
            case Stmt::Kind::defer_: {
                // armed flag: the deferred call runs at exit only if this
                // statement was actually reached (evaluated at exit, like the
                // interpreter does)
                std::string flag = fresh_slot("dfl", cg.t_bool());
                entry_inits += "  store i1 0, ptr " + flag + "\n";
                line("store i1 1, ptr " + flag);
                defers.push_back({s->expr.get(), flag, scopes});
                break;
            }
            case Stmt::Kind::ret: {
                if (is_main) {
                    emit_ret("ret i32 0");
                } else if (s->expr) {
                    EV v = eval(s->expr.get(), ret_ty);
                    emit_ret("ret " + std::string(ll(v.second)) + " " + v.first);
                } else {
                    emit_ret("ret void");
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
            alloc_slot(s->loop_var, cg.t_i64());
            Var* lv = find_var(s->loop_var);
            line("store i64 " + lo.first + ", ptr " + var_ptr(lv));
            std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
            line("br label %" + head);
            label(head);
            std::string cur = var_read(lv), c = reg();
            line(c + " = icmp " + (s->iterable->inclusive ? "sle" : "slt") + " i64 " +
                 cur + ", " + hi.first);
            line("br i1 " + c + ", label %" + body_bb + ", label %" + end);
            label(body_bb);
            loop_stack.push_back({end, step});
            exec_block(s->body);
            loop_stack.pop_back();
            if (!terminated) line("br label %" + step);
            label(step);
            std::string cur2 = var_read(lv), nxt = reg();
            line(nxt + " = add i64 " + cur2 + ", 1");
            line("store i64 " + nxt + ", ptr " + var_ptr(lv));
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
        alloc_slot(s->loop_var, elem);
        Var* lv = find_var(s->loop_var);
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
             var_ptr(lv));
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
        ret_ty = cg.resolve(ret_ref, env, dline, dcol);

        // which locals do closures inside this body capture? those get heap cells
        {
            ClosureScan scan;
            scan.block(body_ref);
            boxed_names = std::move(scan.captured);
        }

        Ty* self_ty = nullptr;
        if (has_self) {
            if (self_impl) self_ty = cg.t_obj(self_impl->mangled);
            else if (self_iface) self_ty = cg.t_obj(self_iface->name, self_iface);
            else if (self_enum) self_ty = cg.t_enum(self_enum->name, {});
        }

        std::string header = "define ";
        header += is_main ? "i32" : ll(ret_ty);
        header += " " + symbol + "(";
        scopes.emplace_back();

        bool first = true;
        if (captured) {
            header += "ptr %env";
            first = false;
        }
        if (self_ty) {
            if (!first) header += ", ";
            header += "ptr %self.arg";
            first = false;
            std::string slot = alloc_slot("self", self_ty, true);
            store_param("%self.arg", slot, "ptr", boxed_names.count("self") > 0);
        }
        for (size_t i = 0; i < params_ref.size(); i++) {
            Ty* pt = cg.resolve(params_ref[i].type.get(), env,
                                params_ref[i].line, params_ref[i].col);
            if (!first) header += ", ";
            first = false;
            std::string preg = "%p" + std::to_string(i);
            header += std::string(ll(pt)) + " " + preg;
            std::string slot = alloc_slot(params_ref[i].name, pt, true);
            store_param(preg, slot, ll(pt), boxed_names.count(params_ref[i].name) > 0);
        }
        header += ") {\nentry:\n";

        // captured cells arrive behind %env: {fnptr @0, cell0 @8, cell1 @16, ...}
        if (captured) {
            for (size_t i = 0; i < captured->size(); i++) {
                const auto& [name, ty] = (*captured)[i];
                std::string slot = "%v" + std::to_string(next_reg++) + "." + name;
                allocas += "  " + slot + " = alloca ptr\n";
                std::string cp = "%cap" + std::to_string(i);
                entry_inits += "  " + cp + " = getelementptr i8, ptr %env, i64 " +
                               std::to_string(8 + 8 * i) + "\n";
                std::string cell = cp + ".c";
                entry_inits += "  " + cell + " = load ptr, ptr " + cp + "\n";
                entry_inits += "  store ptr " + cell + ", ptr " + slot + "\n";
                scopes.back()[name] = {slot, ty, true};
            }
        }

        std::string firstb = bb();
        exec_block(body_ref);
        if (!terminated) {
            if (is_main) emit_ret("ret i32 0");
            else if (ret_ty->k == Ty::unit_) emit_ret("ret void");
            else if (ret_ty->k == Ty::f64_) emit_ret("ret double " + fmt_double(0));
            else if (ret_ty->k == Ty::i64_ || ret_ty->k == Ty::i1_)
                emit_ret("ret " + std::string(ll(ret_ty)) + " 0");
            else emit_ret("ret ptr null");
        }
        scopes.pop_back();

        return header + allocas + entry_inits + "  br label %" + firstb + "\n" +
               firstb + ":\n" + body + "}\n\n";
    }

    // param → its slot; boxed params store into their heap cell instead
    void store_param(const std::string& preg, const std::string& slot,
                     const std::string& lty, bool boxed) {
        if (!boxed) {
            entry_inits += "  store " + lty + " " + preg + ", ptr " + slot + "\n";
            return;
        }
        std::string cell = preg + ".cell";
        entry_inits += "  " + cell + " = load ptr, ptr " + slot + "\n";
        entry_inits += "  store " + lty + " " + preg + ", ptr " + cell + "\n";
    }
};

// ---- CodeGen driver ---------------------------------------------------------

CodeGen::CodeGen(const Module& mod) : mod_(mod) {}

void CodeGen::error_at(uint32_t line, uint32_t col, std::string msg) {
    errors_.push_back({std::move(msg), line, col});
}

std::string CodeGen::generate() {
    CG2 cg(mod_, errors_);

    // top-level functions (generic ones are emitted on demand, per call-site types)
    for (const FnDecl& f : mod_.fns) {
        if (!f.generics.empty()) continue;
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

    // class methods per instantiation + generic fn instances — both queues can
    // grow while we emit, so drain them together
    std::set<std::string> emitted;
    while (true) {
        CImpl* im = nullptr;
        for (CImpl* q : cg.impl_queue) {
            if (!emitted.count(q->mangled)) { im = q; break; }
        }
        if (im) {
            emitted.insert(im->mangled);
            for (const FnDecl& m : im->decl->methods) {
                if (!m.has_body) continue;
                std::string sym = (m.has_self ? "@m_" : "@s_") + im->mangled + "_" + m.name;
                FnEmit fe(cg, m, sym, false, im, nullptr, nullptr, im->env);
                cg.fn_text += fe.emit();
            }
            continue;
        }
        size_t fidx = cg.fn_queue.size();
        for (size_t i = 0; i < cg.fn_queue.size(); i++) {
            if (!cg.fn_emitted.count(cg.fn_queue[i].symbol)) { fidx = i; break; }
        }
        if (fidx == cg.fn_queue.size()) break;
        // copy out — the queue vector can reallocate while this fn emits
        const FnDecl* fdecl = cg.fn_queue[fidx].decl;
        std::map<std::string, Ty*> fenv = cg.fn_queue[fidx].env;
        std::string fsym = cg.fn_queue[fidx].symbol;
        cg.fn_emitted.insert(fsym);
        FnEmit fe(cg, *fdecl, fsym, false, nullptr, nullptr, nullptr, fenv);
        cg.fn_text += fe.emit();
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
    out += "declare double @beans_dec_to_f64(ptr)\n";
    out += "declare i64 @beans_list_max(ptr, i64, ptr)\n";
    out += "declare i64 @beans_list_contains(ptr, i64, i64)\n";
    out += "declare ptr @beans_map_new()\n";
    out += "declare void @beans_map_set(ptr, i64, i64, i64)\n";
    out += "declare i64 @beans_map_get(ptr, i64, i64, ptr)\n";
    out += "declare ptr @beans_thread_spawn(ptr, ptr)\n";
    out += "declare i64 @beans_thread_join(ptr)\n";
    out += "declare ptr @beans_mutex_new(i64)\n";
    out += "declare i64 @beans_mutex_lock(ptr)\n";
    out += "declare void @beans_mutex_unlock(ptr)\n";
    out += "declare ptr @beans_chan_new(i64)\n";
    out += "declare i64 @beans_chan_send(ptr, i64)\n";
    out += "declare i64 @beans_chan_recv(ptr, ptr)\n";
    out += "declare void @beans_chan_close(ptr)\n";
    out += "declare ptr @beans_atomic_new(i64)\n";
    out += "declare i64 @beans_atomic_add(ptr, i64)\n";
    out += "declare i64 @beans_atomic_get(ptr)\n";
    out += "declare void @beans_atomic_set(ptr, i64)\n\n";
    out += cg.globals;
    out += "\n";
    out += tables;
    out += "\n";
    out += cg.fn_text;
    out += cg.lifted;
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

// ---- list search helpers (kind: 0 int-ish, 1 f64, 2 string, 3 decimal) ----
struct BDec;
int beans_dec_cmp(struct BDec* a, struct BDec* b);
static int slot_cmp(long long a, long long b, long long kind) {
    if (kind == 1) {
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind == 2) return strcmp((char*)a, (char*)b);
    if (kind == 3) return beans_dec_cmp((struct BDec*)a, (struct BDec*)b);
    return a < b ? -1 : a > b ? 1 : 0;
}
long long beans_list_max(BList* l, long long kind, long long* ok) {
    *ok = l->len > 0;
    if (!*ok) return 0;
    long long best = l->data[0];
    for (long long i = 1; i < l->len; i++) {
        if (slot_cmp(l->data[i], best, kind) > 0) best = l->data[i];
    }
    return best;
}
long long beans_list_contains(BList* l, long long v, long long kind) {
    for (long long i = 0; i < l->len; i++) {
        if (slot_cmp(l->data[i], v, kind) == 0) return 1;
    }
    return 0;
}

// ---- maps: flat (key, value) pairs; kind 1 = string keys ----
typedef struct {
    long long* data; // key,value interleaved
    long long len, cap;
} BMap;
BMap* beans_map_new(void) {
    BMap* m = calloc(1, sizeof(BMap));
    m->cap = 4;
    m->data = calloc(8, 8);
    return m;
}
static long long map_find(BMap* m, long long key, long long kind) {
    for (long long i = 0; i < m->len; i++) {
        long long k = m->data[i * 2];
        if (kind == 1 ? strcmp((char*)k, (char*)key) == 0 : k == key) return i;
    }
    return -1;
}
void beans_map_set(BMap* m, long long key, long long val, long long kind) {
    long long i = map_find(m, key, kind);
    if (i >= 0) {
        m->data[i * 2 + 1] = val;
        return;
    }
    if (m->len == m->cap) {
        m->cap *= 2;
        m->data = realloc(m->data, (size_t)m->cap * 16);
    }
    m->data[m->len * 2] = key;
    m->data[m->len * 2 + 1] = val;
    m->len += 1;
}
long long beans_map_get(BMap* m, long long key, long long kind, long long* ok) {
    long long i = map_find(m, key, kind);
    *ok = i >= 0;
    return i >= 0 ? m->data[i * 2 + 1] : 0;
}

// ---- threads ----
#include <pthread.h>
typedef struct {
    pthread_t th;
    long long result;
    long long (*thunk)(void*);
    void* env;
    int joined;
} BThread;
static void* thread_main(void* arg) {
    BThread* t = arg;
    t->result = t->thunk(t->env);
    return NULL;
}
BThread* beans_thread_spawn(void* thunk, void* env) {
    BThread* t = calloc(1, sizeof(BThread));
    t->thunk = (long long (*)(void*))thunk;
    t->env = env;
    pthread_create(&t->th, NULL, thread_main, t);
    return t;
}
long long beans_thread_join(BThread* t) {
    if (t->joined) beans_panic("thread already joined", 0, 0);
    t->joined = 1;
    pthread_join(t->th, NULL);
    return t->result;
}

typedef struct {
    pthread_mutex_t m;
    long long inner;
} BMutex;
BMutex* beans_mutex_new(long long inner) {
    BMutex* mu = calloc(1, sizeof(BMutex));
    pthread_mutex_init(&mu->m, NULL);
    mu->inner = inner;
    return mu;
}
long long beans_mutex_lock(BMutex* mu) {
    pthread_mutex_lock(&mu->m);
    return mu->inner;
}
void beans_mutex_unlock(BMutex* mu) { pthread_mutex_unlock(&mu->m); }

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t can_send, can_recv;
    long long* q;
    long long head, count, cap;
    int closed;
} BChan;
BChan* beans_chan_new(long long cap) {
    BChan* c = calloc(1, sizeof(BChan));
    c->cap = cap > 0 ? cap : 1;
    c->q = calloc((size_t)c->cap, 8);
    pthread_mutex_init(&c->m, NULL);
    pthread_cond_init(&c->can_send, NULL);
    pthread_cond_init(&c->can_recv, NULL);
    return c;
}
long long beans_chan_send(BChan* c, long long v) {
    pthread_mutex_lock(&c->m);
    while (c->count == c->cap && !c->closed) pthread_cond_wait(&c->can_send, &c->m);
    if (c->closed) {
        pthread_mutex_unlock(&c->m);
        return 0; // caller panics
    }
    c->q[(c->head + c->count) % c->cap] = v;
    c->count += 1;
    pthread_cond_signal(&c->can_recv);
    pthread_mutex_unlock(&c->m);
    return 1;
}
long long beans_chan_recv(BChan* c, long long* ok) {
    pthread_mutex_lock(&c->m);
    while (c->count == 0 && !c->closed) pthread_cond_wait(&c->can_recv, &c->m);
    if (c->count == 0) {
        *ok = 0;
        pthread_mutex_unlock(&c->m);
        return 0;
    }
    long long v = c->q[c->head];
    c->head = (c->head + 1) % c->cap;
    c->count -= 1;
    *ok = 1;
    pthread_cond_signal(&c->can_send);
    pthread_mutex_unlock(&c->m);
    return v;
}
void beans_chan_close(BChan* c) {
    pthread_mutex_lock(&c->m);
    c->closed = 1;
    pthread_cond_broadcast(&c->can_send);
    pthread_cond_broadcast(&c->can_recv);
    pthread_mutex_unlock(&c->m);
}

typedef struct { _Atomic long long v; } BAtomic;
BAtomic* beans_atomic_new(long long init) {
    BAtomic* a = calloc(1, sizeof(BAtomic));
    a->v = init;
    return a;
}
long long beans_atomic_add(BAtomic* a, long long d) { return (a->v += d); }
long long beans_atomic_get(BAtomic* a) { return a->v; }
void beans_atomic_set(BAtomic* a, long long v) { a->v = v; }

// ---- decimal: 128-bit coefficient + base-10 scale (same math as the interpreter) ----
typedef struct BDec {
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
