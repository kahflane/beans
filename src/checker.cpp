#include "checker.h"

#include <functional>
#include <limits>
#include <utility>

#include "builtins.h"
#include "lexer.h"
#include "parser.h"

namespace beans {

// ---- type display ----------------------------------------------------------

std::string type_name(TypeId t) {
    if (!t) return "?";
    switch (t->k) {
        case Type::K::int_: return "int";
        case Type::K::i8: return "i8";
        case Type::K::i16: return "i16";
        case Type::K::i32: return "i32";
        case Type::K::u8: return "u8";
        case Type::K::u16: return "u16";
        case Type::K::u32: return "u32";
        case Type::K::u64_: return "u64";
        case Type::K::f32: return "f32";
        case Type::K::f64_: return "float";
        case Type::K::decimal_: return "decimal";
        case Type::K::bool_: return "bool";
        case Type::K::string_: return "string";
        case Type::K::unit: return "(nothing)";
        case Type::K::poison: return "<error>";
        case Type::K::type_param: return t->name;
        case Type::K::package: return "package " + t->name;
        case Type::K::range: return "range<" + type_name(t->args[0]) + ">";
        case Type::K::fixed_array:
            return "[" + type_name(t->args[0]) + "; " + t->name + "]";
        case Type::K::fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t->fn_params.size(); i++) {
                if (i) s += ", ";
                s += type_name(t->fn_params[i]);
            }
            s += ")";
            if (t->fn_ret && t->fn_ret->k != Type::K::unit)
                s += " -> " + type_name(t->fn_ret);
            return s;
        }
        case Type::K::class_:
        case Type::K::struct_:
        case Type::K::enum_: {
            std::string s = t->name;
            if (!t->args.empty()) {
                s += "<";
                for (size_t i = 0; i < t->args.size(); i++) {
                    if (i) s += ", ";
                    s += type_name(t->args[i]);
                }
                s += ">";
            }
            return s;
        }
    }
    return "?";
}

// ---- setup ----------------------------------------------------------------

Checker::Checker(const Program& prog)
    : prog_(prog), hir_(prog), pool_(hir_.types()) {}

// abstract registry type -> TypeId; recv only matters for self_recv
TypeId Checker::bt_type(BT t, TypeId recv) {
    switch (t) {
        case BT::unit: return t_unit();
        case BT::i64: return t_int();
        case BT::f64: return t_f64();
        case BT::dec: return t_dec();
        case BT::boolean: return t_bool();
        case BT::str: return t_str();
        case BT::bytes: return pool_.named(Type::K::class_, "Bytes");
        case BT::file: return pool_.named(Type::K::class_, "File");
        case BT::mmap: return pool_.named(Type::K::class_, "MMap");
        case BT::self_recv: return recv ? recv : t_poison();
        case BT::opt_i64: return t_option(t_int());
        case BT::opt_str: return t_option(t_str());
        case BT::list_str: return pool_.named(Type::K::class_, "List", {t_str()});
        case BT::res_i64: return t_result(t_int(), t_error_class());
        case BT::res_f64: return t_result(t_f64(), t_error_class());
        case BT::res_dec: return t_result(t_dec(), t_error_class());
        case BT::res_str: return t_result(t_str(), t_error_class());
        case BT::res_bool: return t_result(t_bool(), t_error_class());
        case BT::res_bytes:
            return t_result(pool_.named(Type::K::class_, "Bytes"), t_error_class());
        case BT::res_file:
            return t_result(pool_.named(Type::K::class_, "File"), t_error_class());
        case BT::res_mmap:
            return t_result(pool_.named(Type::K::class_, "MMap"), t_error_class());
        case BT::res_list_str:
            return t_result(pool_.named(Type::K::class_, "List", {t_str()}),
                            t_error_class());
    }
    return t_poison();
}

void Checker::error_at(uint32_t line, uint32_t col, std::string msg) {
    errors_.push_back({std::move(msg), line, col, cur_file_});
}

// ---- package-aware name resolution -----------------------------------------

std::string Checker::qual(const std::string& name) const {
    return cur_pkg_.empty() ? name : cur_pkg_ + "." + name;
}

void Checker::enter_file(const Package& pkg, const PFile& file) {
    cur_pkg_ = pkg.prefix;
    cur_file_ = prog_.packages.size() > 1 || prog_.module_name.size() ? file.path : "";
    pkg_paths_.clear();
    for (const ImportDecl& i : file.mod.imports) {
        std::string bound = i.alias;
        if (bound.empty()) {
            size_t cut = i.path.find_last_of("./");
            bound = cut == std::string::npos ? i.path : i.path.substr(cut + 1);
        }
        if (pkg_paths_.count(bound)) {
            error_at(i.line, i.col, "import name '" + bound + "' used twice");
        }
        pkg_paths_[bound] = i.path;
    }
}

std::string Checker::import_path_of(const std::string& binding) const {
    auto it = pkg_paths_.find(binding);
    return it == pkg_paths_.end() ? "" : it->second;
}

// key "util.User" -> is it visible from the current package?
bool Checker::check_pub(const std::string& key, bool is_pub, uint32_t line, uint32_t col,
                        const char* what, const std::string& shown) {
    size_t dot = key.find('.');
    std::string owner = dot == std::string::npos ? "" : key.substr(0, dot);
    if (owner == cur_pkg_ || is_pub) return true;
    error_at(line, col, std::string(what) + " '" + shown + "' isn't pub in package '" +
                            owner + "'");
    return false;
}

std::string Checker::resolve_class_key(const std::string& n, uint32_t line, uint32_t col) {
    size_t dot = n.find('.');
    if (dot == std::string::npos) {
        std::string key = qual(n);
        return classes_.count(key) ? key : "";
    }
    std::string path = import_path_of(n.substr(0, dot));
    if (path.empty()) return "";
    auto pit = prefix_by_path_.find(path);
    if (pit == prefix_by_path_.end()) return ""; // std.* or unresolved import
    std::string key = pit->second + "." + n.substr(dot + 1);
    auto it = classes_.find(key);
    if (it == classes_.end()) return "";
    check_pub(key, it->second.decl && it->second.decl->is_pub, line, col, "type", n);
    return key;
}

std::string Checker::resolve_enum_key(const std::string& n, uint32_t line, uint32_t col) {
    size_t dot = n.find('.');
    if (dot == std::string::npos) {
        std::string key = qual(n);
        return enums_.count(key) ? key : "";
    }
    std::string path = import_path_of(n.substr(0, dot));
    if (path.empty()) return "";
    auto pit = prefix_by_path_.find(path);
    if (pit == prefix_by_path_.end()) return "";
    std::string key = pit->second + "." + n.substr(dot + 1);
    auto it = enums_.find(key);
    if (it == enums_.end()) return "";
    check_pub(key, it->second.decl && it->second.decl->is_pub, line, col, "type", n);
    return key;
}

std::string Checker::resolve_fn_key(const std::string& n) {
    std::string key = qual(n);
    return top_fns_.count(key) ? key : "";
}

// `Status` / `util.User` in expression position -> internal key, annotated
std::string Checker::as_type_name(const Expr* e) {
    if (e->kind == Expr::Kind::ident) {
        std::string n(e->text);
        if (find_local(n)) return "";
        std::string key = qual(n);
        if (classes_.count(key) || enums_.count(key)) {
            e->resolved = key;
            return key;
        }
        // builtins keep their plain names
        if (builtin_generic_classes_.count(n) || n == "Error" || n == "Option" ||
            n == "Result" || n == "AtomicInt" || n == "Simd4f32" ||
            n == "Bytes" || n == "File" || n == "Dir" || n == "MMap") {
            return n;
        }
        return "";
    }
    if (e->kind == Expr::Kind::field && e->object->kind == Expr::Kind::ident) {
        std::string base(e->object->text);
        if (find_local(base)) return "";
        std::string path = import_path_of(base);
        if (path.empty()) return "";
        auto pit = prefix_by_path_.find(path);
        if (pit == prefix_by_path_.end()) return "";
        std::string key = pit->second + "." + e->name;
        auto cit = classes_.find(key);
        if (cit != classes_.end()) {
            check_pub(key, cit->second.decl && cit->second.decl->is_pub,
                      e->line, e->col, "type", base + "." + e->name);
            e->resolved = key;
            return key;
        }
        auto eit = enums_.find(key);
        if (eit != enums_.end()) {
            check_pub(key, eit->second.decl && eit->second.decl->is_pub,
                      e->line, e->col, "type", base + "." + e->name);
            e->resolved = key;
            return key;
        }
        return "";
    }
    return "";
}

void Checker::register_builtins() {
    builtin_generic_classes_ = {"List", "Map", "OrderedMap", "Thread", "Mutex", "Channel",
                                "Box", "Arena", "Shared", "Weak", "RawPtr", "Slice"};

    TypeId tp_t = pool_.named(Type::K::type_param, "T");
    TypeId tp_e = pool_.named(Type::K::type_param, "E");

    EnumInfo opt;
    opt.generic_params = {"T"};
    opt.variant_order = {"some", "none"};
    opt.variants["some"] = {tp_t};
    opt.variants["none"] = {};
    enums_["Option"] = std::move(opt);

    EnumInfo res;
    res.generic_params = {"T", "E"};
    res.variant_order = {"ok", "err"};
    res.variants["ok"] = {tp_t};
    res.variants["err"] = {tp_e};
    enums_["Result"] = std::move(res);
}

void Checker::run() {
    register_builtins();
    for (const auto& pkg : prog_.packages) {
        prefix_by_path_[pkg->import_path] = pkg->prefix;
    }
    register_decls();
    for (const auto& pkg : prog_.packages) {
        for (const auto& pf : pkg->files) {
            enter_file(*pkg, *pf);
            for (const ClassDecl& c : pf->mod.classes) {
                auto it = classes_.find(c.qualname);
                if (it != classes_.end() && it->second.decl == &c) check_hierarchy(it->second);
            }
        }
    }
    check_bodies();
}

void Checker::register_decls() {
    // pass 1a: names (whole program, so cross-package references always land)
    for (const auto& pkg : prog_.packages) {
        for (const auto& pf : pkg->files) {
            enter_file(*pkg, *pf);
            for (const ClassDecl& c : pf->mod.classes) {
                if (classes_.count(c.qualname) || enums_.count(c.qualname) ||
                    builtin_generic_classes_.count(c.name) || c.name == "Error") {
                    error_at(c.line, c.col, "type name '" + c.name + "' already taken");
                    continue;
                }
                ClassInfo info;
                info.decl = &c;
                for (const GenericParam& g : c.generics) info.generic_params.push_back(g.name);
                classes_[c.qualname] = std::move(info);
            }
            for (const EnumDecl& e : pf->mod.enums) {
                if (classes_.count(e.qualname) || enums_.count(e.qualname) ||
                    builtin_generic_classes_.count(e.name) || e.name == "Error") {
                    error_at(e.line, e.col, "type name '" + e.name + "' already taken");
                    continue;
                }
                EnumInfo info;
                info.decl = &e;
                for (const GenericParam& g : e.generics) info.generic_params.push_back(g.name);
                enums_[e.qualname] = std::move(info);
            }
        }
    }

    // pass 1b: supers and member signatures, with each file's imports in scope
    for (const auto& pkg : prog_.packages) {
        for (const auto& pf : pkg->files) {
            enter_file(*pkg, *pf);
            for (const ClassDecl& c : pf->mod.classes) {
                auto it = classes_.find(c.qualname);
                if (it != classes_.end() && it->second.decl == &c) {
                    validate_generic_params(c.generics, c.line, c.col);
                    resolve_supers(it->second);
                    resolve_class_members(it->second);
                }
            }
            for (const EnumDecl& e : pf->mod.enums) {
                auto it = enums_.find(e.qualname);
                if (it != enums_.end() && it->second.decl == &e) {
                    validate_generic_params(e.generics, e.line, e.col);
                    resolve_enum_members(it->second);
                }
            }
            for (const FnDecl& f : pf->mod.fns) {
                validate_generic_params(f.generics, f.line, f.col);
                if (top_fns_.count(f.qualname)) {
                    error_at(f.line, f.col, "function '" + f.name + "' defined twice");
                    continue;
                }
                set_type_params(f.generics);
                std::vector<TypeId> params;
                for (const Param& p : f.params) params.push_back(resolve_type(p.type.get()));
                TypeId ret = f.ret ? resolve_type(f.ret.get()) : t_unit();
                if (f.is_extern_c) {
                    if (f.name == "main")
                        error_at(f.line, f.col, "extern function cannot use the reserved name 'main'");
                    if (!f.generics.empty())
                        error_at(f.line, f.col, "extern functions cannot be generic");
                    if (f.params.size() > 6)
                        error_at(f.line, f.col,
                                 "extern C calls support at most 6 arguments for now");
                    if (f.has_body)
                        error_at(f.line, f.col, "extern function is a declaration and has no body");
                    for (size_t i = 0; i < f.params.size(); i++) {
                        if (f.params[i].passing != Param::Passing::borrow) {
                            error_at(f.params[i].line, f.params[i].col,
                                     "extern parameters cannot use move or inout");
                        }
                        if (!is_c_abi_type(params[i], false) &&
                            !is_c_abi_callback(params[i])) {
                            error_at(f.params[i].line, f.params[i].col,
                                     "extern parameter needs an integer, float, bool, RawPtr, extern \"C\" struct/union, or C callback type, got " +
                                         type_name(params[i]));
                        }
                    }
                    if (!is_c_abi_type(ret, true)) {
                        error_at(f.line, f.col,
                                 "extern return needs an integer, float, bool, RawPtr, extern \"C\" struct/union, or no value, got " +
                                     type_name(ret));
                    }
                }
                top_fns_[f.qualname] = pool_.fn(std::move(params), ret);
                top_fn_decls_[f.qualname] = &f;
                cur_type_params_.clear();
                cur_type_bounds_.clear();
            }
        }
    }
    check_inline_layout_cycles();
}

// class parents may be local names or `pkg.Name` — pin them to keys once,
// and write them into the decl for the interpreter and codegen
void Checker::resolve_supers(ClassInfo& c) {
    c.supers.clear();
    c.decl->base_resolved.clear();
    c.decl->interfaces_resolved.clear();
    if (!c.decl->base.empty()) {
        std::string key = resolve_class_key(c.decl->base, c.decl->line, c.decl->col);
        if (key.empty()) {
            error_at(c.decl->line, c.decl->col,
                     "unknown base class '" + c.decl->base + "'");
        } else {
            c.decl->base_resolved = key;
            c.supers.push_back(key);
        }
    }
    for (const std::string& s : c.decl->interfaces) {
        std::string key = resolve_class_key(s, c.decl->line, c.decl->col);
        if (key.empty()) {
            error_at(c.decl->line, c.decl->col, "unknown interface '" + s + "'");
            continue;
        }
        c.supers.push_back(key);
        c.decl->interfaces_resolved.push_back(key);
    }
}

void Checker::resolve_class_members(ClassInfo& c) {
    set_type_params(c.decl->generics);

    for (const FieldDecl& f : c.decl->fields) {
        if (c.fields.count(f.name)) {
            error_at(f.line, f.col, "field '" + f.name + "' defined twice");
            continue;
        }
        TypeId field_type = resolve_type(f.type.get());
        c.fields[f.name] = field_type;
        c.field_decls[f.name] = &f;
        if (c.decl->is_union || c.decl->is_c_layout) {
            bool inline_value = is_inline_storage_type(field_type, c.decl->is_c_layout);
            if (!inline_value && field_type->k != Type::K::poison) {
                error_at(f.line, f.col,
                         "struct/union fields need inline scalar, RawPtr, fixed-array, or nested struct storage, got " +
                             type_name(field_type));
            }
        } else if (c.decl->is_struct && field_type->k == Type::K::unit) {
            error_at(f.line, f.col, "struct fields cannot have unit type");
        }
    }
    for (const FnDecl& m : c.decl->methods) {
        if (c.methods.count(m.name) || c.fields.count(m.name)) {
            error_at(m.line, m.col, "member '" + m.name + "' defined twice");
            continue;
        }
        validate_generic_params(m.generics, m.line, m.col);
        auto saved_params = cur_type_params_;
        auto saved_bounds = cur_type_bounds_;
        for (const GenericParam& g : m.generics) {
            if (!cur_type_params_.insert(g.name).second) {
                error_at(m.line, m.col, "type parameter '" + g.name +
                                            "' hides an outer type parameter");
            }
            const auto& bounds = g.bounds_resolved.size() == g.bounds.size()
                                     ? g.bounds_resolved : g.bounds;
            for (const std::string& trait : bounds)
                cur_type_bounds_[g.name].insert(trait);
        }
        std::vector<TypeId> params;
        for (const Param& p : m.params) params.push_back(resolve_type(p.type.get()));
        TypeId ret = m.ret ? resolve_type(m.ret.get()) : t_unit();
        c.methods[m.name] = pool_.fn(std::move(params), ret);
        c.method_decls[m.name] = &m;
        cur_type_params_ = std::move(saved_params);
        cur_type_bounds_ = std::move(saved_bounds);
    }
    cur_type_params_.clear();
    cur_type_bounds_.clear();
}

void Checker::check_inline_layout_cycles() {
    enum class State : uint8_t { unseen, visiting, done };
    std::map<std::string, State> state;
    std::function<void(const std::string&)> visit = [&](const std::string& key) {
        auto it = classes_.find(key);
        if (it == classes_.end() || !it->second.decl ||
            (!it->second.decl->is_struct && !it->second.decl->is_union))
            return;
        if (state[key] == State::done) return;
        state[key] = State::visiting;
        for (const auto& [name, original] : it->second.fields) {
            TypeId field = original;
            while (field && field->k == Type::K::fixed_array && !field->args.empty())
                field = field->args[0];
            if (!field || field->k != Type::K::struct_) continue;
            if (state[field->name] == State::visiting) {
                const FieldDecl* decl = it->second.field_decls.at(name);
                error_at(decl->line, decl->col,
                         "recursive inline layout through field '" + name +
                             "' has no finite size — use RawPtr or Box for the edge");
                continue;
            }
            visit(field->name);
        }
        state[key] = State::done;
    };
    for (const auto& [key, info] : classes_) {
        if (info.decl && (info.decl->is_struct || info.decl->is_union)) visit(key);
    }
}

void Checker::resolve_enum_members(EnumInfo& e) {
    set_type_params(e.decl->generics);

    for (const EnumVariant& v : e.decl->variants) {
        if (e.variants.count(v.name)) {
            error_at(e.decl->line, e.decl->col, "variant '" + v.name + "' defined twice");
            continue;
        }
        std::vector<TypeId> payload;
        for (const Param& p : v.payload) {
            TypeId type = resolve_type(p.type.get());
            payload.push_back(type);
        }
        e.variants[v.name] = std::move(payload);
        e.variant_order.push_back(v.name);
    }
    for (const FnDecl& m : e.decl->methods) {
        if (m.name == "init" || m.name == "deinit") {
            // enums are values, not identities — nothing to construct or tear down
            error_at(m.line, m.col, "'" + m.name + "' belongs to classes, not enums");
            continue;
        }
        if (e.methods.count(m.name)) {
            error_at(m.line, m.col, "method '" + m.name + "' defined twice");
            continue;
        }
        validate_generic_params(m.generics, m.line, m.col);
        auto saved_params = cur_type_params_;
        auto saved_bounds = cur_type_bounds_;
        for (const GenericParam& g : m.generics) {
            if (!cur_type_params_.insert(g.name).second) {
                error_at(m.line, m.col, "type parameter '" + g.name +
                                            "' hides an outer type parameter");
            }
            const auto& bounds = g.bounds_resolved.size() == g.bounds.size()
                                     ? g.bounds_resolved : g.bounds;
            for (const std::string& trait : bounds)
                cur_type_bounds_[g.name].insert(trait);
        }
        std::vector<TypeId> params;
        for (const Param& p : m.params) params.push_back(resolve_type(p.type.get()));
        TypeId ret = m.ret ? resolve_type(m.ret.get()) : t_unit();
        e.methods[m.name] = pool_.fn(std::move(params), ret);
        e.method_decls[m.name] = &m;
        cur_type_params_ = std::move(saved_params);
        cur_type_bounds_ = std::move(saved_bounds);
    }
    cur_type_params_.clear();
    cur_type_bounds_.clear();
}

// ---- hierarchy -------------------------------------------------------------

bool Checker::is_subclass_of(const std::string& cls, const std::string& super) {
    if (cls == super) return true;
    std::set<std::string> seen;
    std::vector<std::string> work = {cls};
    while (!work.empty()) {
        std::string n = work.back();
        work.pop_back();
        if (!seen.insert(n).second) continue;
        auto it = classes_.find(n);
        if (it == classes_.end()) continue;
        for (const std::string& s : it->second.supers) {
            if (s == super) return true;
            work.push_back(s);
        }
    }
    return false;
}

const FnDecl* Checker::class_init(const ClassInfo& c) {
    auto it = c.method_decls.find("init");
    return it != c.method_decls.end() && it->second->has_self ? it->second : nullptr;
}

const ClassInfo* Checker::parent_class_of(const ClassInfo& c) {
    for (const std::string& s : c.supers) {
        auto it = classes_.find(s);
        if (it != classes_.end() && !it->second.decl->is_interface) return &it->second;
    }
    return nullptr;
}

const FnDecl* Checker::chain_init(const ClassInfo& c, const ClassInfo** owner) {
    for (const ClassInfo* k = &c; k; k = parent_class_of(*k)) {
        if (const FnDecl* ini = class_init(*k)) {
            if (owner) *owner = k;
            return ini;
        }
    }
    if (owner) *owner = nullptr;
    return nullptr;
}

void Checker::check_hierarchy(ClassInfo& c) {
    const ClassDecl* d = c.decl;
    int class_parents = 0;

    if (d->is_struct || d->is_union) {
        if (d->fields.empty())
            error_at(d->line, d->col,
                     d->is_union ? "unions need at least one field"
                                 : "structs need at least one field");
        if (!d->generics.empty())
            error_at(d->line, d->col,
                     d->is_union ? "generic unions are not available yet"
                                 : "generic structs are not available yet");
        if (!d->base.empty() || !d->interfaces.empty())
            error_at(d->line, d->col,
                     d->is_union ? "unions cannot inherit" : "structs cannot inherit");
        for (const FnDecl& method : d->methods)
            error_at(method.line, method.col,
                     d->is_union ? "union methods are not available yet"
                                 : "struct methods are not available yet");
        if (d->is_union) {
            for (const FieldDecl& field : d->fields) {
                if (field.def)
                    error_at(field.line, field.col,
                             "union fields cannot have defaults — initialize one field explicitly");
            }
        }
        return;
    }

    // ---- init / deinit shape rules ----
    for (const auto& [mname, mdecl] : c.method_decls) {
        bool is_init = mname == "init";
        if (!is_init && mname != "deinit") continue;
        if (d->is_interface) {
            error_at(mdecl->line, mdecl->col,
                     "'" + mname + "' can't be declared in an interface");
            continue;
        }
        if (!mdecl->has_self)
            error_at(mdecl->line, mdecl->col,
                     "'" + mname + "' is an instance method — remove 'static'");
        if (mdecl->ret) {
            error_at(mdecl->line, mdecl->col, "'" + mname + "' returns nothing");
        }
        if (!mdecl->generics.empty()) {
            error_at(mdecl->line, mdecl->col, "'" + mname + "' can't take type parameters");
        }
        if (mdecl->is_override) {
            error_at(mdecl->line, mdecl->col,
                     is_init ? "init can't be marked override"
                             : "deinit chains to the parent automatically — drop the override");
        }
        if (!is_init && !mdecl->params.empty()) {
            error_at(mdecl->line, mdecl->col, "deinit takes no parameters");
        }
    }

    // collect inherited members
    std::map<std::string, TypeId> inh_concrete;   // concrete method or iface default
    std::map<std::string, bool> inh_concrete_self;
    std::map<std::string, TypeId> inh_sigs;       // iface signatures without bodies
    std::map<std::string, const FnDecl*> inh_decls;

    std::set<std::string> seen = {d->qualname}; // supers are qualified keys
    std::vector<std::string> work = c.supers;
    if (!d->base_resolved.empty()) {
        auto bit = classes_.find(d->base_resolved);
        if (bit != classes_.end() && bit->second.decl &&
            bit->second.decl->is_interface)
            error_at(d->line, d->col, "extends needs a class; put interfaces after implements");
    }
    for (const std::string& name : d->interfaces_resolved) {
        auto iit = classes_.find(name);
        if (iit != classes_.end() && iit->second.decl &&
            !iit->second.decl->is_interface)
            error_at(d->line, d->col,
                     d->is_interface ? "interfaces may extend only interfaces"
                                     : "implements needs an interface");
    }
    for (const std::string& s : c.supers) {
        auto it = classes_.find(s);
        if (it == classes_.end()) {
            error_at(d->line, d->col, "unknown parent '" + s + "'");
            continue;
        }
        if (!it->second.decl->is_interface) class_parents += 1;
    }
    if (class_parents > 1) {
        error_at(d->line, d->col, "only one class parent allowed — the rest must be interfaces");
    }

    while (!work.empty()) {
        std::string n = work.back();
        work.pop_back();
        if (!seen.insert(n).second) {
            if (n == d->qualname) error_at(d->line, d->col, "inheritance cycle involving '" + n + "'");
            continue;
        }
        auto it = classes_.find(n);
        if (it == classes_.end()) continue;
        const ClassInfo& p = it->second;
        for (const auto& [mname, mtype] : p.methods) {
            const FnDecl* md = p.method_decls.at(mname);
            if (!md->has_self) continue; // statics don't participate
            if (md->has_body) {
                if (!inh_concrete.count(mname)) {
                    inh_concrete[mname] = mtype;
                    inh_concrete_self[mname] = true;
                    inh_decls[mname] = md;
                }
            } else {
                if (!inh_sigs.count(mname)) {
                    inh_sigs[mname] = mtype;
                    inh_decls[mname] = md;
                }
            }
        }
        for (const std::string& s : p.supers) work.push_back(s);
    }

    // constructor chain completeness: with an init somewhere above, every
    // class either declares its own init (and calls super.init) or adds no
    // required fields, so the inherited constructor still covers everything
    if (!d->is_interface && !class_init(c)) {
        const ClassInfo* p = parent_class_of(c);
        if (p && chain_init(*p, nullptr)) {
            for (const FieldDecl& f : d->fields) {
                if (!f.def) {
                    error_at(d->line, d->col,
                             "'" + d->name + "' adds required field '" + f.name +
                                 "' but has no init — declare fn init(...) and "
                                     "call super.init(...)");
                }
            }
        } else {
            for (const FieldDecl& f : d->fields) {
                if (!f.def) {
                    error_at(d->line, d->col,
                             "class '" + d->name + "' has required field '" + f.name +
                                 "' — declare fn init(...); only all-default classes "
                                 "receive an implicit init()");
                }
            }
        }
    }

    // override rules
    for (const auto& [mname, mdecl] : c.method_decls) {
        if (!mdecl->has_self) continue;
        TypeId own = c.methods.at(mname);
        bool has_concrete = inh_concrete.count(mname) > 0;
        bool has_sig = inh_sigs.count(mname) > 0;
        if ((has_concrete || has_sig) && inh_decls.count(mname)) {
            const FnDecl* parent_decl = inh_decls[mname];
            for (size_t i = 0; i < mdecl->params.size() &&
                               i < parent_decl->params.size(); i++) {
                if (mdecl->params[i].passing != parent_decl->params[i].passing) {
                    error_at(mdecl->line, mdecl->col,
                             "override of '" + mname +
                                 "' changes ownership mode of argument " +
                                 std::to_string(i + 1));
                }
            }
        }
        if (mdecl->is_override) {
            if (!has_concrete && !has_sig) {
                error_at(mdecl->line, mdecl->col,
                         "'" + mname + "' is marked override but no parent has it");
            } else if (has_concrete && inh_concrete[mname] != own) {
                error_at(mdecl->line, mdecl->col,
                         "override of '" + mname + "' changes the signature: parent has " +
                             type_name(inh_concrete[mname]) + ", this is " + type_name(own));
            } else if (!has_concrete && has_sig && inh_sigs[mname] != own) {
                error_at(mdecl->line, mdecl->col,
                         "'" + mname + "' doesn't match the interface: expected " +
                             type_name(inh_sigs[mname]) + ", this is " + type_name(own));
            }
        } else {
            if (has_concrete && mname != "deinit" && mname != "init") {
                // deinit never overrides (subclass runs, then parent's), and a
                // subclass init calls super.init — both are composition
                error_at(mdecl->line, mdecl->col,
                         "'" + mname + "' hides an inherited method — mark it override");
            } else if (has_sig && inh_sigs[mname] != own) {
                error_at(mdecl->line, mdecl->col,
                         "'" + mname + "' doesn't match the interface: expected " +
                             type_name(inh_sigs[mname]) + ", this is " + type_name(own));
            }
        }
    }

    // interface completeness (real classes only)
    if (!d->is_interface) {
        for (const auto& [mname, mtype] : inh_sigs) {
            bool have = c.methods.count(mname) || inh_concrete.count(mname);
            if (!have) {
                error_at(d->line, d->col,
                         "class '" + d->name + "' is missing interface method '" + mname +
                             "' " + type_name(mtype));
            }
        }
    }
}

// ---- types -----------------------------------------------------------------

TypeId Checker::resolve_type(const TypeRef* t) {
    if (!t) return t_poison();

    if (t->kind == TypeRef::Kind::fixed_array) {
        TypeId elem = resolve_type(t->array_elem.get());
        if (!is_inline_storage_type(elem, false) && elem->k != Type::K::poison) {
            error_at(t->line, t->col,
                     "fixed arrays need inline scalar, RawPtr, fixed-array, or struct elements, got " +
                         type_name(elem));
        }
        if (t->array_len == 0 || t->array_len > 4096) {
            error_at(t->line, t->col,
                     "fixed array length must be between 1 and 4096");
        }
        return pool_.fixed_array_of(elem, t->array_len);
    }

    if (t->kind == TypeRef::Kind::fn) {
        std::vector<TypeId> params;
        for (const TypePtr& p : t->fn_params) params.push_back(resolve_type(p.get()));
        TypeId ret = t->fn_ret ? resolve_type(t->fn_ret.get()) : t_unit();
        return pool_.fn(std::move(params), ret);
    }

    const std::string& n = t->name;
    size_t nargs = t->args.size();

    auto want_args = [&](size_t want) {
        if (nargs != want) {
            error_at(t->line, t->col,
                     "'" + n + "' takes " + std::to_string(want) + " type argument(s), got " +
                         std::to_string(nargs));
            return false;
        }
        return true;
    };
    auto resolved_args = [&]() {
        std::vector<TypeId> out;
        for (const TypePtr& a : t->args) out.push_back(resolve_type(a.get()));
        return out;
    };

    if (cur_type_params_.count(n)) {
        if (nargs) error_at(t->line, t->col, "type parameter '" + n + "' takes no arguments");
        return pool_.named(Type::K::type_param, n);
    }

    if (nargs == 0) {
        if (n == "int" || n == "i64") return pool_.prim(Type::K::int_);
        if (n == "i8") return pool_.prim(Type::K::i8);
        if (n == "i16") return pool_.prim(Type::K::i16);
        if (n == "i32") return pool_.prim(Type::K::i32);
        if (n == "u8" || n == "byte") return pool_.prim(Type::K::u8);
        if (n == "u16") return pool_.prim(Type::K::u16);
        if (n == "u32") return pool_.prim(Type::K::u32);
        if (n == "u64") return pool_.prim(Type::K::u64_);
        if (n == "f32") return pool_.prim(Type::K::f32);
        if (n == "f64" || n == "float") return pool_.prim(Type::K::f64_);
        if (n == "decimal") return pool_.prim(Type::K::decimal_);
        if (n == "bool") return pool_.prim(Type::K::bool_);
        if (n == "string") return pool_.prim(Type::K::string_);
        if (n == "Error") return t_error_class();
        if (n == "AtomicInt") return pool_.named(Type::K::class_, "AtomicInt");
        if (n == "Simd4f32") return pool_.named(Type::K::class_, "Simd4f32");
        if (n == "Bytes") return pool_.named(Type::K::class_, "Bytes");
        if (n == "File") return pool_.named(Type::K::class_, "File");
        if (n == "MMap") return pool_.named(Type::K::class_, "MMap");
    }

    if (n == "Option") {
        if (!want_args(1)) return t_poison();
        return pool_.named(Type::K::enum_, "Option", resolved_args());
    }
    if (n == "Result") {
        if (nargs == 1) {
            auto args = resolved_args();
            args.push_back(t_error_class());
            return pool_.named(Type::K::enum_, "Result", std::move(args));
        }
        if (!want_args(2)) return t_poison();
        return pool_.named(Type::K::enum_, "Result", resolved_args());
    }
    if (builtin_generic_classes_.count(n)) {
        size_t want = (n == "Map" || n == "OrderedMap") ? 2 : 1;
        if (!want_args(want)) return t_poison();
        std::vector<TypeId> args = resolved_args();
        if ((n != "Map" && n != "OrderedMap") &&
                   n != "RawPtr" && n != "Slice" && n != "List" &&
                   n != "Box" && n != "Arena" && n != "Shared" &&
                   n != "Weak" && n != "Mutex" && n != "Channel" &&
                   n != "Thread") {
            for (TypeId arg : args) {
                if (is_wide_inline_value(arg)) {
                    error_at(t->line, t->col,
                             "wide inline values are not supported in runtime-slot containers yet");
                }
            }
        }
        if ((n == "Map" || n == "OrderedMap") && args.size() == 2) {
            if (!trait_satisfied(args[0], "Eq"))
                error_at(t->line, t->col, n + " key needs Eq, got " + type_name(args[0]));
            if (!trait_satisfied(args[0], "Hash"))
                error_at(t->line, t->col, n + " key needs Hash, got " + type_name(args[0]));
        }
        if ((n == "RawPtr" || n == "Slice") && args.size() == 1 &&
            !is_raw_pointee(args[0])) {
            error_at(t->line, t->col,
                     n + " only supports inline scalars, RawPtr, fixed arrays, and extern \"C\" struct/union values, got " +
                         type_name(args[0]));
        }
        return pool_.named(Type::K::class_, n, std::move(args));
    }

    std::string ckey = resolve_class_key(n, t->line, t->col);
    if (!ckey.empty()) {
        auto cit = classes_.find(ckey);
        if (!want_args(cit->second.generic_params.size())) return t_poison();
        t->resolved = ckey;
        std::vector<TypeId> args = resolved_args();
        std::map<std::string, TypeId> env;
        for (size_t i = 0; i < cit->second.decl->generics.size() && i < args.size(); i++)
            env[cit->second.decl->generics[i].name] = args[i];
        check_generic_bounds(cit->second.decl->generics, env, t->line, t->col,
                             "type '" + n + "'");
        return pool_.named((cit->second.decl->is_struct || cit->second.decl->is_union)
                               ? Type::K::struct_
                               : Type::K::class_,
                           ckey, std::move(args));
    }
    std::string ekey = resolve_enum_key(n, t->line, t->col);
    if (!ekey.empty()) {
        auto eit = enums_.find(ekey);
        if (!want_args(eit->second.generic_params.size())) return t_poison();
        t->resolved = ekey;
        std::vector<TypeId> args = resolved_args();
        std::map<std::string, TypeId> env;
        for (size_t i = 0; i < eit->second.decl->generics.size() && i < args.size(); i++)
            env[eit->second.decl->generics[i].name] = args[i];
        check_generic_bounds(eit->second.decl->generics, env, t->line, t->col,
                             "type '" + n + "'");
        return pool_.named(Type::K::enum_, ekey, std::move(args));
    }

    error_at(t->line, t->col, "unknown type '" + n + "'");
    return t_poison();
}

TypeId Checker::subst(TypeId t, const std::map<std::string, TypeId>& map) {
    if (!t || map.empty()) return t;
    switch (t->k) {
        case Type::K::type_param: {
            auto it = map.find(t->name);
            return it != map.end() ? it->second : t;
        }
        case Type::K::class_:
        case Type::K::struct_:
        case Type::K::enum_: {
            if (t->args.empty()) return t;
            std::vector<TypeId> args;
            for (TypeId a : t->args) args.push_back(subst(a, map));
            return pool_.named(t->k, t->name, std::move(args));
        }
        case Type::K::range:
            return pool_.range_of(subst(t->args[0], map));
        case Type::K::fixed_array:
            return pool_.fixed_array_of(subst(t->args[0], map),
                                        static_cast<uint32_t>(std::stoul(t->name)));
        case Type::K::fn: {
            std::vector<TypeId> params;
            for (TypeId p : t->fn_params) params.push_back(subst(p, map));
            return pool_.fn(std::move(params), subst(t->fn_ret, map));
        }
        default:
            return t;
    }
}

bool Checker::unify(TypeId param, TypeId arg, std::map<std::string, TypeId>& out) {
    if (!param || !arg) return false;
    if (param->k == Type::K::type_param) {
        auto it = out.find(param->name);
        if (it != out.end()) return it->second == arg;
        out[param->name] = arg;
        return true;
    }
    if (param->k != arg->k) return param == arg;
    switch (param->k) {
        case Type::K::class_:
        case Type::K::struct_:
        case Type::K::enum_: {
            if (param->name != arg->name || param->args.size() != arg->args.size()) return false;
            for (size_t i = 0; i < param->args.size(); i++) {
                if (!unify(param->args[i], arg->args[i], out)) return false;
            }
            return true;
        }
        case Type::K::range:
            return unify(param->args[0], arg->args[0], out);
        case Type::K::fixed_array:
            return param->name == arg->name &&
                   unify(param->args[0], arg->args[0], out);
        case Type::K::fn: {
            if (param->fn_params.size() != arg->fn_params.size()) return false;
            for (size_t i = 0; i < param->fn_params.size(); i++) {
                if (!unify(param->fn_params[i], arg->fn_params[i], out)) return false;
            }
            return unify(param->fn_ret, arg->fn_ret, out);
        }
        default:
            return param == arg;
    }
}

bool Checker::assignable(TypeId from, TypeId to) {
    if (!from || !to) return true;
    if (from->k == Type::K::poison || to->k == Type::K::poison) return true;
    if (from == to) return true;
    // implicit upcast to a parent class or interface (non-generic targets)
    if (from->k == Type::K::class_ && to->k == Type::K::class_ && to->args.empty()) {
        return is_subclass_of(from->name, to->name);
    }
    return false;
}

bool Checker::printable(TypeId t) {
    std::set<TypeId> seen;
    return printable_rec(t, seen);
}

// lists print as [a, b], enums as variant(payload...) — printable when every
// piece is. Class payloads stay out: their display would need the dynamic
// class name, which the native backend does not carry. That excludes Result.
bool Checker::printable_rec(TypeId t, std::set<TypeId>& seen) {
    if (!t) return false;
    if (t->k == Type::K::poison) return true;
    if (t->is_numeric() || t->k == Type::K::bool_ || t->k == Type::K::string_) return true;
    if (t->k == Type::K::class_ && t->name == "List" && t->args.size() == 1) {
        return printable_rec(t->args[0], seen);
    }
    if (t->k == Type::K::enum_) {
        if (seen.count(t)) return true; // self-recursive enums hold finite values
        seen.insert(t);
        auto it = enums_.find(t->name);
        if (it == enums_.end()) return false;
        const EnumInfo& ei = it->second;
        std::map<std::string, TypeId> env;
        for (size_t i = 0; i < ei.generic_params.size() && i < t->args.size(); i++) {
            env[ei.generic_params[i]] = t->args[i];
        }
        for (const auto& [vn, payload] : ei.variants) {
            for (TypeId p : payload) {
                if (!printable_rec(subst(p, env), seen)) return false;
            }
        }
        return true;
    }
    return false;
}

std::string Checker::canonical_trait(const std::string& trait) {
    return trait;
}

void Checker::set_type_params(const std::vector<GenericParam>& params) {
    cur_type_params_.clear();
    cur_type_bounds_.clear();
    for (const GenericParam& param : params) {
        cur_type_params_.insert(param.name);
        const auto& bounds = param.bounds_resolved.size() == param.bounds.size()
                                 ? param.bounds_resolved : param.bounds;
        for (const std::string& trait : bounds)
            cur_type_bounds_[param.name].insert(trait);
    }
}

void Checker::validate_generic_params(const std::vector<GenericParam>& params,
                                      uint32_t line, uint32_t col) {
    static const std::set<std::string> known = {
        "Clone", "Eq", "Hash", "Order", "Send", "Sync",
    };
    std::set<std::string> names;
    for (const GenericParam& param : params) {
        if (!names.insert(param.name).second) {
            error_at(line, col, "type parameter '" + param.name + "' is declared twice");
        }
        param.bounds_resolved.clear();
        for (const std::string& trait : param.bounds) {
            if (known.count(trait)) {
                param.bounds_resolved.push_back(trait);
                continue;
            }
            std::string key = resolve_class_key(trait, line, col);
            auto it = classes_.find(key);
            if (key.empty() || it == classes_.end() || !it->second.decl ||
                !it->second.decl->is_interface) {
                error_at(line, col, "generic bound '" + trait + "' is not an interface");
                continue;
            }
            param.bounds_resolved.push_back(key);
        }
    }
}

bool Checker::trait_satisfied(TypeId t, const std::string& trait) {
    std::set<std::pair<TypeId, std::string>> seen;
    return trait_satisfied_rec(t, canonical_trait(trait), seen);
}

bool Checker::trait_satisfied_rec(TypeId t, const std::string& raw_trait,
                                  std::set<std::pair<TypeId, std::string>>& seen) {
    if (!t || t->k == Type::K::poison) return true;
    const std::string trait = canonical_trait(raw_trait);
    if (!seen.insert({t, trait}).second) return true;

    if (t->k == Type::K::type_param) {
        auto it = cur_type_bounds_.find(t->name);
        if (it == cur_type_bounds_.end()) return false;
        if (it->second.count(trait)) return true;
        for (const std::string& bound : it->second) {
            auto bit = classes_.find(bound);
            if (bit != classes_.end() && bit->second.decl &&
                bit->second.decl->is_interface && is_subclass_of(bound, trait))
                return true;
        }
        // A total order includes equality.
        return trait == "Eq" && it->second.count("Order");
    }

    auto requested = classes_.find(trait);
    if (requested != classes_.end() && requested->second.decl &&
        requested->second.decl->is_interface) {
        return t->k == Type::K::class_ && is_subclass_of(t->name, trait);
    }

    bool scalar = t->is_numeric() || t->k == Type::K::bool_ ||
                  t->k == Type::K::string_ || t->k == Type::K::unit;
    if (scalar) {
        return trait == "Clone" || trait == "Eq" || trait == "Hash" ||
               trait == "Order" || trait == "Send" || trait == "Sync";
    }

    if (t->k == Type::K::range) {
        return (trait == "Clone" || trait == "Send" || trait == "Sync") &&
               trait_satisfied_rec(t->args[0], trait, seen);
    }

    if (t->k == Type::K::fixed_array) {
        return (trait == "Clone" || trait == "Eq" || trait == "Hash" ||
                trait == "Send" || trait == "Sync") &&
               trait_satisfied_rec(t->args[0], trait, seen);
    }

    if (t->k == Type::K::fn) return trait == "Clone";

    if (t->k == Type::K::struct_) {
        auto it = classes_.find(t->name);
        if (it == classes_.end() || !it->second.decl ||
            (!it->second.decl->is_struct && !it->second.decl->is_union))
            return false;
        if (it->second.decl->is_union) return trait == "Clone";
        if (trait != "Clone" && trait != "Eq" && trait != "Hash" &&
            trait != "Send" && trait != "Sync")
            return false;
        for (const auto& [name, field] : it->second.fields) {
            (void)name;
            if (!trait_satisfied_rec(field, trait, seen)) return false;
        }
        return true;
    }

    if (t->k == Type::K::enum_) {
        if (trait == "Order") return false;
        if (trait != "Clone" && trait != "Eq" && trait != "Hash" &&
            trait != "Send" && trait != "Sync")
            return false;
        auto it = enums_.find(t->name);
        if (it == enums_.end()) return false;
        std::map<std::string, TypeId> env;
        for (size_t i = 0; i < it->second.generic_params.size() && i < t->args.size(); i++)
            env[it->second.generic_params[i]] = t->args[i];
        for (const auto& [variant, payload] : it->second.variants) {
            (void)variant;
            for (TypeId field : payload) {
                if (!trait_satisfied_rec(subst(field, env), trait, seen)) return false;
            }
        }
        return true;
    }

    if (t->k != Type::K::class_) return false;

    const std::string& name = t->name;
    if (name == "List") {
        if (trait == "Eq" || trait == "Hash") return true; // identity today
        return trait == "Clone" && t->args.size() == 1 && !is_move_only(t->args[0]) &&
               trait_satisfied_rec(t->args[0], "Clone", seen);
    }
    if (name == "Map" || name == "OrderedMap") {
        if (trait != "Clone" || t->args.size() != 2) return false;
        return !is_move_only(t->args[0]) && !is_move_only(t->args[1]) &&
               trait_satisfied_rec(t->args[0], "Clone", seen) &&
               trait_satisfied_rec(t->args[1], "Clone", seen);
    }
    if (name == "Box" || name == "Arena") {
        return false;
    }
    if (name == "Shared" || name == "Weak") {
        if (trait == "Clone") return true;
        if ((trait == "Send" || trait == "Sync") && t->args.size() == 1) {
            return trait_satisfied_rec(t->args[0], "Send", seen) &&
                   trait_satisfied_rec(t->args[0], "Sync", seen);
        }
        return false;
    }
    if (name == "Mutex") {
        // Mutex is the synchronization boundary for local ARC values. This is
        // needed until inout lets value structs be mutated under the lock.
        return trait == "Clone" || trait == "Send" || trait == "Sync";
    }
    if (name == "Channel") {
        if (trait == "Clone") return true;
        return (trait == "Send" || trait == "Sync") && t->args.size() == 1 &&
               trait_satisfied_rec(t->args[0], "Send", seen);
    }
    if (name == "Thread") {
        if (trait == "Clone") return true;
        return trait == "Send" && t->args.size() == 1 &&
               trait_satisfied_rec(t->args[0], "Send", seen);
    }
    if (name == "AtomicInt") {
        return trait == "Clone" || trait == "Send" || trait == "Sync";
    }
    if (name == "Simd4f32") {
        return trait == "Clone" || trait == "Send" || trait == "Sync";
    }
    if (name == "RawPtr") {
        return trait == "Clone" || trait == "Eq" || trait == "Hash" ||
               trait == "Send" || trait == "Sync";
    }
    if (name == "Slice") {
        return trait == "Clone" || trait == "Send" || trait == "Sync";
    }
    if (name == "Bytes") {
        return trait == "Clone" || trait == "Eq" || trait == "Hash";
    }
    if (name == "File" || name == "MMap") {
        return trait == "Clone";
    }

    // Ordinary classes are local ARC references. Copy/equality/hash are
    // identity operations, but the reference cannot cross a thread boundary.
    if (classes_.count(name) || name == "Error") {
        return (trait == "Clone" && !is_move_only(t)) || trait == "Eq" ||
               trait == "Hash";
    }
    return false;
}

void Checker::check_generic_bounds(const std::vector<GenericParam>& params,
                                   const std::map<std::string, TypeId>& env,
                                   uint32_t line, uint32_t col,
                                   const std::string& what) {
    for (const GenericParam& param : params) {
        auto it = env.find(param.name);
        if (it == env.end()) continue;
        const auto& bounds = param.bounds_resolved.size() == param.bounds.size()
                                 ? param.bounds_resolved : param.bounds;
        for (const std::string& trait : bounds) {
            if (!trait_satisfied(it->second, trait)) {
                error_at(line, col, what + " needs " + param.name + " implements " +
                                        trait + ", got " + type_name(it->second));
            }
        }
    }
}

bool Checker::is_move_only(TypeId t) const {
    if (!t) return false;
    if (t->k == Type::K::fixed_array && !t->args.empty())
        return is_move_only(t->args[0]);
    if (t->k == Type::K::class_ &&
        (t->name == "Box" || t->name == "Arena" || t->name == "List" ||
         t->name == "Map" || t->name == "OrderedMap"))
        return true;
    if (t->k == Type::K::class_) {
        std::set<std::string> seen;
        std::vector<std::string> work = {t->name};
        while (!work.empty()) {
            std::string name = std::move(work.back());
            work.pop_back();
            if (!seen.insert(name).second) continue;
            auto it = classes_.find(name);
            if (it == classes_.end()) continue;
            if (it->second.decl && it->second.decl->is_move_only) return true;
            for (const std::string& parent : it->second.supers) work.push_back(parent);
        }
    }
    if (t->k == Type::K::struct_) {
        auto it = classes_.find(t->name);
        if (it != classes_.end() && it->second.decl &&
            it->second.decl->is_move_only)
            return true;
        if (it != classes_.end()) {
            for (const auto& [name, field] : it->second.fields) {
                (void)name;
                if (is_move_only(field)) return true;
            }
        }
    }
    if (t->k == Type::K::enum_) {
        for (TypeId arg : t->args) {
            if (is_move_only(arg)) return true;
        }
    }
    return false;
}

bool Checker::is_inline_storage_type(TypeId t, bool require_c_layout) const {
    if (!t) return false;
    if (t->is_int() || t->is_float() || t->k == Type::K::bool_) return true;
    if (t->k == Type::K::class_ && t->name == "RawPtr" && t->args.size() == 1)
        return true;
    if (t->k == Type::K::fixed_array && t->args.size() == 1)
        return is_inline_storage_type(t->args[0], require_c_layout);
    if (t->k != Type::K::struct_) return false;
    auto it = classes_.find(t->name);
    if (it == classes_.end() || !it->second.decl ||
        (!it->second.decl->is_struct && !it->second.decl->is_union))
        return false;
    return !require_c_layout || it->second.decl->is_c_layout;
}

bool Checker::is_wide_inline_value(TypeId t) const {
    if (!t) return false;
    if (t->k == Type::K::struct_ || t->k == Type::K::fixed_array) return true;
    if (t->k == Type::K::class_ &&
        (t->name == "Simd4f32" || t->name == "Slice"))
        return true;
    if (t->k != Type::K::enum_) return false;
    if (t->name == "Option" && t->args.size() == 1)
        return is_wide_inline_value(t->args[0]);
    return t->name == "Result" && t->args.size() == 2 &&
           (is_wide_inline_value(t->args[0]) || is_wide_inline_value(t->args[1]));
}

bool Checker::is_raw_pointee(TypeId t) const {
    return is_inline_storage_type(t, true);
}

bool Checker::is_c_abi_type(TypeId t, bool allow_unit) const {
    if (!t) return false;
    if (allow_unit && t->k == Type::K::unit) return true;
    if (t->is_int() || t->is_float() || t->k == Type::K::bool_) return true;
    if (t->k == Type::K::class_ && t->name == "RawPtr" && t->args.size() == 1)
        return true;
    if (t->k != Type::K::struct_) return false;
    auto it = classes_.find(t->name);
    return it != classes_.end() && it->second.decl &&
           (it->second.decl->is_struct || it->second.decl->is_union) &&
           it->second.decl->is_c_layout && it->second.decl->generics.empty();
}

bool Checker::is_c_abi_callback(TypeId t) const {
    if (!t || t->k != Type::K::fn || t->fn_params.size() > 6) return false;
    for (TypeId parameter : t->fn_params)
        if (!is_c_abi_type(parameter, false)) return false;
    return is_c_abi_type(t->fn_ret, true);
}

void Checker::require_move_source(const Expr* e, TypeId t, const std::string& where) {
    if (!e || !is_move_only(t)) return;
    if (e->kind == Expr::Kind::unary && e->op == TokenKind::kw_move) return;
    if (e->kind == Expr::Kind::ident) {
        error_at(e->line, e->col,
                 where + " needs 'move " + std::string(e->text) +
                     "' because " + type_name(t) + " is move-only");
    } else if (e->kind == Expr::Kind::field || e->kind == Expr::Kind::index) {
        error_at(e->line, e->col,
                 where + " cannot move a field or index yet — move it through a local");
    }
}

// ---- scopes ----------------------------------------------------------------

void Checker::declare(const std::string& name, TypeId t, bool mut,
                      uint32_t line, uint32_t col, bool borrowed) {
    if (!scopes_.empty() && scopes_.back().count(name)) {
        error_at(line, col, "'" + name + "' is already defined in this scope");
        return;
    }
    scopes_.back()[name] = {t, mut, borrowed, false, MoveState::available};
}

Checker::Local* Checker::find_local_mut(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

int Checker::local_scope_index(const std::string& name) const {
    for (size_t i = scopes_.size(); i-- > 0;) {
        if (scopes_[i].count(name)) return static_cast<int>(i);
    }
    return -1;
}

void Checker::merge_move_states(
    const std::vector<std::map<std::string, Local>>& a,
    const std::vector<std::map<std::string, Local>>& b) {
    for (size_t i = 0; i < scopes_.size(); i++) {
        if (i >= a.size() || i >= b.size()) continue;
        for (auto& [name, local] : scopes_[i]) {
            auto ai = a[i].find(name);
            auto bi = b[i].find(name);
            if (ai == a[i].end() || bi == b[i].end()) continue;
            MoveState x = ai->second.move;
            MoveState y = bi->second.move;
            local.move = x == y ? x : MoveState::maybe_moved;
        }
    }
}

const Checker::Local* Checker::find_local(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

// ---- bodies ----------------------------------------------------------------

void Checker::check_bodies() {
    for (const auto& pkg : prog_.packages) {
        for (const auto& pf : pkg->files) {
            enter_file(*pkg, *pf);
            for (const ClassDecl& cd : pf->mod.classes) {
                auto cit = classes_.find(cd.qualname);
                if (cit == classes_.end() || cit->second.decl != &cd) continue;
                ClassInfo& c = cit->second;
                for (const FnDecl& m : cd.methods) check_fn_body(m, &c, nullptr);
                // field defaults
                cur_class_ = &c;
                set_type_params(cd.generics);
                scopes_.clear();
                push_scope();
                for (const FieldDecl& f : cd.fields) {
                    if (!f.def) continue;
                    TypeId want = c.fields.count(f.name) ? c.fields[f.name] : t_poison();
                    TypeId got = check_expr(f.def.get(), want);
                    if (!assignable(got, want)) {
                        error_at(f.line, f.col, "default value for '" + f.name + "' is " +
                                                    type_name(got) + ", field is " +
                                                    type_name(want));
                    }
                }
                pop_scope();
                cur_class_ = nullptr;
                cur_type_params_.clear();
                cur_type_bounds_.clear();
            }
            for (const EnumDecl& ed : pf->mod.enums) {
                auto eit = enums_.find(ed.qualname);
                if (eit == enums_.end() || eit->second.decl != &ed) continue;
                for (const FnDecl& m : ed.methods) check_fn_body(m, nullptr, &eit->second);
            }
            for (const FnDecl& f : pf->mod.fns) check_fn_body(f, nullptr, nullptr);
        }
    }
}

TypeId Checker::class_type_of(const ClassInfo& c) {
    std::vector<TypeId> args;
    for (const std::string& g : c.generic_params)
        args.push_back(pool_.named(Type::K::type_param, g));
    return pool_.named((c.decl->is_struct || c.decl->is_union)
                           ? Type::K::struct_
                           : Type::K::class_,
                       c.decl->qualname, std::move(args));
}

void Checker::check_fn_body(const FnDecl& f, ClassInfo* cls, EnumInfo* en) {
    if (!f.has_body) return;

    take_floor_depth_ = -1;
    capture_floor_depth_ = -1;
    in_defer_ = false;
    require_send_captures_ = false;
    bad_send_captures_.clear();
    allow_inout_expr_ = false;
    bad_inout_captures_.clear();
    unsafe_depth_ = 0;

    // the constructor's straight-line-prefix proof runs before type checking,
    // so "assign the fields first" lands ahead of any knock-on type errors
    if (cls && f.has_self && f.name == "init") check_init_body(f, *cls);

    cur_class_ = cls;
    cur_enum_ = en;
    cur_has_self_ = f.has_self;
    in_init_body_ = cls && f.has_self && f.name == "init";
    std::vector<GenericParam> type_params;
    if (cls) type_params.insert(type_params.end(), cls->decl->generics.begin(),
                                cls->decl->generics.end());
    if (en) type_params.insert(type_params.end(), en->decl->generics.begin(),
                              en->decl->generics.end());
    type_params.insert(type_params.end(), f.generics.begin(), f.generics.end());
    set_type_params(type_params);

    cur_ret_ = f.ret ? resolve_type(f.ret.get()) : t_unit();

    scopes_.clear();
    push_scope();
    for (const Param& p : f.params) {
        declare(p.name, resolve_type(p.type.get()),
                p.passing == Param::Passing::inout, p.line, p.col,
                p.passing != Param::Passing::move);
        if (p.passing == Param::Passing::inout) {
            if (Local* local = find_local_mut(p.name)) local->inout = true;
        }
    }
    check_block(f.body);
    pop_scope();

    if (f.ret && cur_ret_ != t_unit() && cur_ret_->k != Type::K::poison &&
        !always_returns(f.body)) {
        error_at(f.line, f.col, "'" + f.name + "' must return " + type_name(cur_ret_) +
                                    " — the body can finish without a return");
    }

    cur_class_ = nullptr;
    cur_enum_ = nullptr;
    cur_has_self_ = false;
    in_init_body_ = false;
    cur_ret_ = nullptr;
    cur_type_params_.clear();
    cur_type_bounds_.clear();
}

void Checker::check_block(const std::vector<StmtPtr>& body) {
    push_scope();
    block_depth_ += 1;
    for (const StmtPtr& s : body) check_stmt(s.get());
    block_depth_ -= 1;
    pop_scope();
}

// ---- missing return --------------------------------------------------------

// beans has no implicit tail-expression return — a value-returning function
// says `return` (SYNTAX.md, "Functions"). So a `-> T` body that can run off the
// end has no value to hand back, and the two backends each invented a different
// one: unit in the interpreter, the type's zero value in codegen, which for a
// pointer type was a null the caller then dereferenced. Rejecting it here is
// what keeps the two agreeing.
//
// The walk is deliberately conservative: unsure means "does not return", which
// at worst asks for a `return` the reader can already see is needed.

// A `break` binds to the innermost loop, so this stops at a nested loop instead
// of counting its breaks as this loop's.
bool Checker::has_break(const std::vector<StmtPtr>& body) {
    for (const StmtPtr& s : body) {
        switch (s->kind) {
            case Stmt::Kind::brk:
                return true;
            case Stmt::Kind::if_:
                if (has_break(s->body) || has_break(s->else_body)) return true;
                break;
            case Stmt::Kind::unsafe_:
                if (has_break(s->body)) return true;
                break;
            case Stmt::Kind::expr:
                if (s->expr && s->expr->kind == Expr::Kind::match_expr) {
                    for (const MatchArm& a : s->expr->arms) {
                        if (a.is_block && has_break(a.body)) return true;
                    }
                }
                break;
            default:
                break;
        }
    }
    return false;
}

bool Checker::stmt_returns(const Stmt* s) {
    switch (s->kind) {
        case Stmt::Kind::ret:
            return true;
        case Stmt::Kind::if_:
            // no else and the false path falls straight through
            return !s->else_body.empty() && always_returns(s->body) &&
                   always_returns(s->else_body);
        case Stmt::Kind::for_ever:
            // `for { }` with no break never finishes, so nothing follows it
            return !has_break(s->body);
        case Stmt::Kind::unsafe_:
            return always_returns(s->body);
        case Stmt::Kind::expr:
            // a statement-position match counts when every arm returns —
            // check_match already proved the arms cover the subject
            if (s->expr && s->expr->kind == Expr::Kind::match_expr &&
                !s->expr->arms.empty()) {
                for (const MatchArm& a : s->expr->arms) {
                    if (!a.is_block || !always_returns(a.body)) return false;
                }
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool Checker::always_returns(const std::vector<StmtPtr>& body) {
    // any returning statement is enough — whatever follows it is unreachable
    for (const StmtPtr& s : body) {
        if (stmt_returns(s.get())) return true;
    }
    return false;
}

void Checker::check_stmt(const Stmt* s) {
    switch (s->kind) {
        case Stmt::Kind::let_: {
            TypeId declared = resolve_type(s->type.get());
            if (!s->init) {
                error_at(s->line, s->col,
                         (s->is_var ? std::string("var") : std::string("let")) +
                             " '" + s->name + "' needs a starting value");
            } else {
                TypeId got = check_expr(s->init.get(), declared);
                require_move_source(s->init.get(), got, "binding '" + s->name + "'");
                if (!assignable(got, declared)) {
                    error_at(s->line, s->col, "'" + s->name + "' is declared " +
                                                  type_name(declared) + " but the value is " +
                                                  type_name(got));
                }
            }
            declare(s->name, declared, s->is_var, s->line, s->col);
            break;
        }
        case Stmt::Kind::assign: {
            TypeId target_t = t_poison();
            Local* target_local = nullptr;
            const Expr* t = s->target.get();
            if (t->kind == Expr::Kind::ident) {
                Local* l = find_local_mut(std::string(t->text));
                if (!l) {
                    error_at(t->line, t->col, "unknown name '" + std::string(t->text) + "'");
                } else {
                    if (!l->mut) {
                        error_at(t->line, t->col, "'" + std::string(t->text) +
                                                      "' is a let — it can't be reassigned. use var");
                    }
                    target_t = l->type;
                    target_local = l;
                    if (s->op != TokenKind::assign && l->move != MoveState::available) {
                        error_at(t->line, t->col,
                                 l->move == MoveState::moved
                                     ? "use of moved value '" + std::string(t->text) + "'"
                                     : "value '" + std::string(t->text) +
                                           "' may have been moved");
                    }
                }
            } else if (t->kind == Expr::Kind::field) {
                target_t = check_expr(t, nullptr);
                TypeId owner = hir_.type_of(t->object.get());
                if (owner && owner->k == Type::K::struct_) {
                    auto owner_info = classes_.find(owner->name);
                    if (s->op != TokenKind::assign && owner_info != classes_.end() &&
                        owner_info->second.decl && owner_info->second.decl->is_union) {
                        error_at(t->line, t->col,
                                 "union fields only support direct assignment for now");
                    }
                    if (t->object->kind != Expr::Kind::ident) {
                        error_at(t->line, t->col,
                                 "struct field assignment needs a local variable for now");
                    } else if (Local* local =
                                   find_local_mut(std::string(t->object->text));
                               local && !local->mut) {
                        error_at(t->line, t->col,
                                 "'" + std::string(t->object->text) +
                                     "' is a let — its fields can't be reassigned. use var");
                    }
                }
            } else if (t->kind == Expr::Kind::index) {
                TypeId owner = check_expr(t->object.get(), nullptr);
                if (owner->k == Type::K::fixed_array && owner->args.size() == 1) {
                    TypeId idx = check_expr(t->index_expr.get(), t_int());
                    if (!idx->is_int() && idx->k != Type::K::poison)
                        error_at(t->line, t->col, "fixed array index must be an integer");
                    if (t->object->kind != Expr::Kind::ident) {
                        error_at(t->line, t->col,
                                 "fixed array element assignment needs a local variable for now");
                    } else if (Local* local =
                                   find_local_mut(std::string(t->object->text));
                               local && !local->mut) {
                        error_at(t->line, t->col,
                                 "'" + std::string(t->object->text) +
                                     "' is a let — its elements can't be reassigned. use var");
                    }
                    target_t = owner->args[0];
                } else if (owner->k == Type::K::class_ && owner->name == "List" &&
                    owner->args.size() == 1) {
                    TypeId idx = check_expr(t->index_expr.get(), t_int());
                    if (!idx->is_int() && idx->k != Type::K::poison)
                        error_at(t->line, t->col, "list index must be an integer");
                    target_t = owner->args[0];
                } else if (owner->k == Type::K::class_ &&
                           (owner->name == "Map" || owner->name == "OrderedMap") &&
                           owner->args.size() == 2) {
                    TypeId key = check_expr(t->index_expr.get(), owner->args[0]);
                    if (!assignable(key, owner->args[0])) {
                        error_at(t->line, t->col, "map key is " + type_name(owner->args[0]) +
                                                      ", got " + type_name(key));
                    }
                    require_move_source(t->index_expr.get(), owner->args[0],
                                        "map index assignment");
                    target_t = owner->args[1];
                } else if (owner->k != Type::K::poison) {
                    error_at(t->line, t->col, "can't index into " + type_name(owner));
                }
                hir_.set_type(t, target_t);
            } else {
                error_at(t->line, t->col, "can't assign to this expression");
            }

            TypeId val = check_expr(s->value.get(), target_t);
            if (s->op == TokenKind::assign) {
                require_move_source(s->value.get(), val, "assignment");
                if (!assignable(val, target_t)) {
                    error_at(s->line, s->col, "can't assign " + type_name(val) + " to " +
                                                  type_name(target_t));
                }
                if (target_local && target_local->mut)
                    target_local->move = MoveState::available;
            } else {
                // compound: numeric, same type on both sides
                if (target_t->k != Type::K::poison &&
                    (!target_t->is_numeric() || val != target_t)) {
                    if (target_t->k == Type::K::string_) {
                        error_at(s->line, s->col,
                                 "no += on strings — build strings with interpolation \"{a}{b}\"");
                    } else if (val->k != Type::K::poison) {
                        error_at(s->line, s->col, std::string(to_string(s->op)) + " needs " +
                                                      type_name(target_t) + " on both sides, got " +
                                                      type_name(val));
                    }
                }
            }
            break;
        }
        case Stmt::Kind::expr:
            if (s->expr && s->expr->kind == Expr::Kind::match_expr) {
                // statement match: arms may be blocks, values are dropped
                check_match(s->expr.get(), nullptr, true);
            } else {
                check_expr(s->expr.get(), nullptr);
            }
            break;
        case Stmt::Kind::ret: {
            if (cur_ret_ == t_unit()) {
                if (s->expr) error_at(s->line, s->col, "this function doesn't return a value");
                else break;
            }
            if (!s->expr) {
                if (cur_ret_ != t_unit())
                    error_at(s->line, s->col, "return needs a " + type_name(cur_ret_));
                break;
            }
            TypeId got = check_expr(s->expr.get(), cur_ret_);
            require_move_source(s->expr.get(), got, "return");
            if (!assignable(got, cur_ret_)) {
                error_at(s->line, s->col, "returning " + type_name(got) +
                                              " from a function that returns " + type_name(cur_ret_));
            }
            break;
        }
        case Stmt::Kind::brk:
        case Stmt::Kind::cont:
            break;
        case Stmt::Kind::if_: {
            TypeId c = check_expr(s->cond.get(), t_bool());
            if (c != t_bool() && c->k != Type::K::poison) {
                error_at(s->cond->line, s->cond->col,
                         "condition must be bool, got " + type_name(c));
            }
            auto base = scopes_;
            check_block(s->body);
            auto yes = scopes_;
            scopes_ = base;
            if (!s->else_body.empty()) check_block(s->else_body);
            auto no = scopes_;
            scopes_ = base;
            bool yes_returns = always_returns(s->body);
            bool no_returns = !s->else_body.empty() && always_returns(s->else_body);
            if (yes_returns && !no_returns) scopes_ = no;
            else if (!yes_returns && no_returns) scopes_ = yes;
            else if (!yes_returns && !no_returns) merge_move_states(yes, no);
            break;
        }
        case Stmt::Kind::for_ever: {
            auto base = scopes_;
            int saved_floor = take_floor_depth_;
            take_floor_depth_ = static_cast<int>(scopes_.size());
            check_block(s->body);
            take_floor_depth_ = saved_floor;
            scopes_ = base;
            break;
        }
        case Stmt::Kind::for_while: {
            auto base = scopes_;
            int saved_floor = take_floor_depth_;
            take_floor_depth_ = static_cast<int>(scopes_.size());
            TypeId c = check_expr(s->cond.get(), t_bool());
            if (c != t_bool() && c->k != Type::K::poison) {
                error_at(s->cond->line, s->cond->col,
                         "loop condition must be bool, got " + type_name(c));
            }
            check_block(s->body);
            take_floor_depth_ = saved_floor;
            scopes_ = base;
            break;
        }
        case Stmt::Kind::for_in: {
            TypeId it = check_expr(s->iterable.get(), nullptr);
            TypeId elem = t_poison();
            if (it->k == Type::K::range) {
                elem = it->args[0];
            } else if (it->k == Type::K::fixed_array && it->args.size() == 1) {
                elem = it->args[0];
            } else if (it->k == Type::K::class_ && it->name == "Slice" &&
                       it->args.size() == 1) {
                if (unsafe_depth_ == 0)
                    error_at(s->iterable->line, s->iterable->col,
                             "looping over Slice requires unsafe { }");
                elem = it->args[0];
            } else if (it->k == Type::K::class_ && it->name == "List" && it->args.size() == 1) {
                elem = it->args[0];
            } else if (it->k != Type::K::poison) {
                error_at(s->iterable->line, s->iterable->col,
                         "can't loop over " + type_name(it) +
                             " — expected a fixed array, List, or range");
            }
            TypeId declared = resolve_type(s->loop_type.get());
            if (elem->k != Type::K::poison && declared != elem &&
                declared->k != Type::K::poison) {
                error_at(s->line, s->col, "loop variable is " + type_name(declared) +
                                              " but elements are " + type_name(elem));
            }
            auto base = scopes_; // a take in the iterable happens once and sticks
            int saved_floor = take_floor_depth_;
            take_floor_depth_ = static_cast<int>(scopes_.size());
            push_scope();
            declare(s->loop_var, declared, false, s->line, s->col, true);
            check_block(s->body);
            pop_scope();
            take_floor_depth_ = saved_floor;
            scopes_ = base;
            break;
        }
        case Stmt::Kind::defer_:
            // a function-exit hook, not a block-exit one: nested registration
            // would need runtime capture the native backend does not have, so
            // the rule is one shape both backends implement identically
            if (block_depth_ != 1) {
                error_at(s->line, s->col,
                         "defer must sit at the top level of a function body");
            }
            {
                bool saved_defer = in_defer_;
                in_defer_ = true;
                check_expr(s->expr.get(), nullptr);
                in_defer_ = saved_defer;
            }
            break;
        case Stmt::Kind::unsafe_:
            unsafe_depth_ += 1;
            check_block(s->body);
            unsafe_depth_ -= 1;
            break;
    }
}

// ---- expressions -----------------------------------------------------------

bool Checker::is_adaptable_literal(const Expr* e) {
    if (!e) return false;
    if (e->kind == Expr::Kind::int_lit || e->kind == Expr::Kind::float_lit) return true;
    if (e->kind == Expr::Kind::unary && e->op == TokenKind::minus)
        return is_adaptable_literal(e->rhs.get());
    return false;
}

TypeId Checker::literal_or(const Expr* e, TypeId expected, TypeId dflt) {
    TypeId t = dflt;
    if (expected) {
        if (e->kind == Expr::Kind::int_lit && expected->is_numeric()) t = expected;
        if (e->kind == Expr::Kind::float_lit &&
            (expected->is_float() || expected->k == Type::K::decimal_))
            t = expected;
    }
    // stamp the decision on the node — the interpreter reads it back so a
    // literal in a decimal/float spot builds the same Value the checker typed
    e->numk = t->k == Type::K::decimal_ ? 3 : t->is_float() ? 2 : 1;
    if (e->kind == Expr::Kind::int_lit && t->is_int()) check_integer_literal(e, t);
    return t;
}

namespace {

bool parse_uint_literal(std::string_view text, unsigned __int128& value) {
    int base = 10;
    size_t pos = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        pos = 2;
    } else if (text.size() > 2 && text[0] == '0' &&
               (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        pos = 2;
    }
    value = 0;
    bool any = false;
    const unsigned __int128 max = ~static_cast<unsigned __int128>(0);
    for (; pos < text.size(); pos++) {
        char c = text[pos];
        if (c == '_') continue;
        unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
                         : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                         : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                : static_cast<unsigned>(base);
        if (digit >= static_cast<unsigned>(base)) return false;
        if (value > (max - digit) / static_cast<unsigned>(base)) return false;
        value = value * static_cast<unsigned>(base) + digit;
        any = true;
    }
    return any;
}

std::string uint128_string(unsigned __int128 value) {
    if (value == 0) return "0";
    std::string out;
    while (value) {
        out.insert(out.begin(), static_cast<char>('0' + value % 10));
        value /= 10;
    }
    return out;
}

} // namespace

void Checker::check_integer_literal(const Expr* e, TypeId type) {
    IntLayout layout = hir_.target().integer(type->k);
    if (!layout.bits) return;
    unsigned __int128 magnitude = 0;
    bool parsed = parse_uint_literal(e->text, magnitude);
    bool negative = literal_sign_ < 0;
    unsigned __int128 limit = 0;
    if (layout.is_signed) {
        limit = negative ? (static_cast<unsigned __int128>(1) << (layout.bits - 1))
                         : (static_cast<unsigned __int128>(1) << (layout.bits - 1)) - 1;
    } else {
        limit = layout.bits == 64
                    ? static_cast<unsigned __int128>(std::numeric_limits<uint64_t>::max())
                    : (static_cast<unsigned __int128>(1) << layout.bits) - 1;
    }
    bool fits = parsed && magnitude <= limit && (!negative || layout.is_signed || magnitude == 0);
    if (fits || !bad_integer_literals_.insert(e).second) return;

    std::string range;
    if (layout.is_signed) {
        unsigned __int128 edge = static_cast<unsigned __int128>(1) << (layout.bits - 1);
        range = "-" + uint128_string(edge) + ".." + uint128_string(edge - 1);
    } else {
        range = "0.." + uint128_string(limit);
    }
    error_at(e->line, e->col,
             "integer literal " + std::string(negative ? "-" : "") +
                 std::string(e->text) + " does not fit " + type_name(type) + " (" + range + ")");
}

TypeId Checker::check_expr(const Expr* e, TypeId expected) {
    TypeId type = check_expr_impl(e, expected);
    hir_.set_type(e, type);
    return type;
}

TypeId Checker::check_expr_impl(const Expr* e, TypeId expected) {
    if (!e) return t_poison();
    switch (e->kind) {
        case Expr::Kind::int_lit:
            return literal_or(e, expected, t_int());
        case Expr::Kind::float_lit:
            return literal_or(e, expected, t_f64());
        case Expr::Kind::string_lit:
            check_interpolation(e);
            return t_str();
        case Expr::Kind::bool_lit:
            return t_bool();
        case Expr::Kind::ident: {
            std::string name(e->text);
            if (Local* l = find_local_mut(name)) {
                bool captured = capture_floor_depth_ >= 0 &&
                                local_scope_index(name) < capture_floor_depth_;
                if (captured)
                    l->borrowed = true;
                if (captured && l->inout &&
                    bad_inout_captures_.insert(name).second) {
                    error_at(e->line, e->col,
                             "closure cannot capture inout parameter '" + name + "'");
                }
                if (captured && require_send_captures_ &&
                    !trait_satisfied(l->type, "Send") &&
                    bad_send_captures_.insert(name).second) {
                    error_at(e->line, e->col,
                             "thread closure cannot capture '" + name + "' of non-Send type " +
                                 type_name(l->type));
                }
                if (l->move == MoveState::moved) {
                    error_at(e->line, e->col, "use of moved value '" + name + "'");
                } else if (l->move == MoveState::maybe_moved) {
                    error_at(e->line, e->col,
                             "value '" + name + "' may have been moved");
                }
                return l->type;
            }
            std::string fkey = resolve_fn_key(name);
            if (!fkey.empty()) {
                e->resolved = fkey;
                if (top_fn_decls_[fkey]->is_extern_c) {
                    error_at(e->line, e->col,
                             "extern C function '" + name +
                                 "' cannot be stored as a Beans function value yet");
                }
                for (const Param& p : top_fn_decls_[fkey]->params) {
                    if (p.passing != Param::Passing::borrow) {
                        error_at(e->line, e->col,
                                 "function '" + name +
                                     "' has ownership parameters and cannot be stored as a value yet");
                        break;
                    }
                }
                return top_fns_[fkey];
            }
            if (name == "none") {
                if (expected && expected->k == Type::K::enum_ && expected->name == "Option")
                    return expected;
                error_at(e->line, e->col,
                         "can't tell which Option this 'none' is — the spot needs a declared type");
                return t_poison();
            }
            if (classes_.count(qual(name)) || enums_.count(qual(name)) ||
                builtin_generic_classes_.count(name)) {
                error_at(e->line, e->col, "'" + name + "' is a type, not a value");
                return t_poison();
            }
            if (pkg_paths_.count(name)) {
                error_at(e->line, e->col, "'" + name + "' is a package, not a value");
                return t_poison();
            }
            error_at(e->line, e->col, "unknown name '" + name + "'");
            return t_poison();
        }
        case Expr::Kind::self_ref: {
            if (capture_floor_depth_ >= 0 && require_send_captures_ && cur_class_ &&
                bad_send_captures_.insert("self").second) {
                error_at(e->line, e->col,
                         "thread closure cannot capture self — class references are non-Send");
            }
            if (cur_class_ && cur_has_self_) return class_type_of(*cur_class_);
            if (cur_enum_ && cur_has_self_) {
                std::vector<TypeId> args;
                for (const std::string& g : cur_enum_->generic_params)
                    args.push_back(pool_.named(Type::K::type_param, g));
                return pool_.named(Type::K::enum_, cur_enum_->decl->qualname, std::move(args));
            }
            error_at(e->line, e->col, "self isn't available here");
            return t_poison();
        }
        case Expr::Kind::unary: {
            if (e->op == TokenKind::kw_inout) {
                if (!allow_inout_expr_) {
                    error_at(e->line, e->col,
                             "inout is only valid for an inout call argument");
                }
                if (!e->rhs || e->rhs->kind != Expr::Kind::ident) {
                    error_at(e->line, e->col, "inout needs a mutable local name");
                    return t_poison();
                }
                std::string name(e->rhs->text);
                Local* local = find_local_mut(name);
                if (!local) {
                    error_at(e->rhs->line, e->rhs->col, "unknown local '" + name + "'");
                    return t_poison();
                }
                hir_.set_type(e->rhs.get(), local->type);
                if (!local->mut) {
                    error_at(e->line, e->col,
                             "inout needs var, but '" + name + "' is a let");
                }
                if (local->move == MoveState::moved) {
                    error_at(e->line, e->col, "use of moved value '" + name + "'");
                } else if (local->move == MoveState::maybe_moved) {
                    error_at(e->line, e->col, "value '" + name + "' may have been moved");
                }
                return local->type;
            }
            if (e->op == TokenKind::kw_move) {
                if (!e->rhs || e->rhs->kind != Expr::Kind::ident) {
                    error_at(e->line, e->col, "move needs a local name");
                    return t_poison();
                }
                std::string name(e->rhs->text);
                Local* local = find_local_mut(name);
                if (!local) {
                    error_at(e->rhs->line, e->rhs->col,
                             "unknown local '" + name + "'");
                    return t_poison();
                }
                TypeId type = local->type;
                hir_.set_type(e->rhs.get(), type);
                if (local->move == MoveState::moved) {
                    error_at(e->line, e->col, "value '" + name + "' was already moved");
                    return type;
                }
                if (local->move == MoveState::maybe_moved) {
                    error_at(e->line, e->col,
                             "value '" + name + "' may already have been moved");
                    return type;
                }
                if (local->borrowed) {
                    error_at(e->line, e->col,
                             "can't move borrowed binding '" + name + "'");
                    return type;
                }
                int depth = local_scope_index(name);
                if (take_floor_depth_ >= 0 && depth < take_floor_depth_) {
                    error_at(e->line, e->col,
                             "can't move outer value '" + name +
                                 "' from a loop or escaping closure");
                    return type;
                }
                if (in_defer_) {
                    error_at(e->line, e->col, "move is not allowed inside defer");
                    return type;
                }
                local->move = MoveState::moved;
                return type;
            }
            if (e->op == TokenKind::minus) {
                bool literal = is_adaptable_literal(e->rhs.get()) &&
                               e->rhs->kind != Expr::Kind::float_lit;
                if (literal) literal_sign_ = -literal_sign_;
                TypeId t = check_expr(e->rhs.get(), expected && expected->is_numeric() ? expected : nullptr);
                if (literal) literal_sign_ = -literal_sign_;
                if (!t->is_numeric() && t->k != Type::K::poison) {
                    error_at(e->line, e->col, "unary '-' needs a number, got " + type_name(t));
                    return t_poison();
                }
                return t;
            }
            if (e->op == TokenKind::bang) {
                TypeId t = check_expr(e->rhs.get(), t_bool());
                if (t != t_bool() && t->k != Type::K::poison) {
                    error_at(e->line, e->col, "'!' needs a bool, got " + type_name(t));
                }
                return t_bool();
            }
            // ~
            TypeId t = check_expr(e->rhs.get(), nullptr);
            if (!t->is_int() && t->k != Type::K::poison) {
                error_at(e->line, e->col, "'~' needs an integer, got " + type_name(t));
                return t_poison();
            }
            return t;
        }
        case Expr::Kind::binary:
            return check_binary(e);
        case Expr::Kind::range: {
            TypeId l = check_expr(e->lhs.get(), nullptr);
            TypeId r = check_expr(e->rhs.get(), l);
            if (l != r && is_adaptable_literal(e->lhs.get())) {
                l = check_expr(e->lhs.get(), r);
            }
            if (l->k == Type::K::poison || r->k == Type::K::poison) return t_poison();
            if (!l->is_int() || l != r) {
                error_at(e->line, e->col, "ranges need the same integer type on both ends, got " +
                                              type_name(l) + " and " + type_name(r));
                return t_poison();
            }
            return pool_.range_of(l);
        }
        case Expr::Kind::new_:
            return check_new(e, expected);
        case Expr::Kind::call:
            return check_call(e, expected);
        case Expr::Kind::field:
            return check_field(e, false, nullptr);
        case Expr::Kind::index: {
            TypeId obj = check_expr(e->object.get(), nullptr);
            if (obj->k == Type::K::class_ && obj->name == "Slice" &&
                obj->args.size() == 1) {
                if (unsafe_depth_ == 0)
                    error_at(e->line, e->col, "Slice indexing requires unsafe { }");
                TypeId idx = check_expr(e->index_expr.get(), t_int());
                if (!idx->is_int() && idx->k != Type::K::poison)
                    error_at(e->line, e->col, "slice index must be an integer");
                return obj->args[0];
            }
            if (obj->k == Type::K::fixed_array && obj->args.size() == 1) {
                TypeId idx = check_expr(e->index_expr.get(), t_int());
                if (!idx->is_int() && idx->k != Type::K::poison)
                    error_at(e->line, e->col, "fixed array index must be an integer");
                return obj->args[0];
            }
            if (obj->k == Type::K::class_ && obj->name == "List" && obj->args.size() == 1) {
                TypeId idx = check_expr(e->index_expr.get(), t_int());
                if (!idx->is_int() && idx->k != Type::K::poison) {
                    error_at(e->line, e->col, "list index must be an integer");
                }
                if (is_move_only(obj->args[0])) {
                    error_at(e->line, e->col,
                             "can't copy a move-only list element by index — use remove");
                    return t_poison();
                }
                return obj->args[0];
            }
            if (obj->k == Type::K::class_ &&
                (obj->name == "Map" || obj->name == "OrderedMap") &&
                obj->args.size() == 2) {
                TypeId key = check_expr(e->index_expr.get(), obj->args[0]);
                if (!assignable(key, obj->args[0])) {
                    error_at(e->line, e->col, "map key is " + type_name(obj->args[0]) +
                                                  ", got " + type_name(key));
                }
                if (is_move_only(obj->args[1])) {
                    error_at(e->line, e->col,
                             "can't copy a move-only map value by index — a consuming map read is not available yet");
                    return t_poison();
                }
                return obj->args[1];
            }
            if (obj->k != Type::K::poison) {
                error_at(e->line, e->col, "can't index into " + type_name(obj));
            }
            return t_poison();
        }
        case Expr::Kind::list_lit: {
            TypeId elem_expected = nullptr;
            bool fixed = expected && expected->k == Type::K::fixed_array &&
                         expected->args.size() == 1;
            if (fixed) elem_expected = expected->args[0];
            if (expected && expected->k == Type::K::class_ && expected->name == "List" &&
                expected->args.size() == 1) {
                elem_expected = expected->args[0];
            }
            TypeId elem = elem_expected;
            for (const ExprPtr& el : e->args) {
                TypeId t = check_expr(el.get(), elem);
                require_move_source(el.get(), t, "list literal");
                if (!elem) {
                    elem = t;
                } else if (!assignable(t, elem)) {
                    error_at(el->line, el->col, "list of " + type_name(elem) +
                                                    " can't hold a " + type_name(t));
                }
            }
            if (!elem) {
                error_at(e->line, e->col,
                         "can't tell the element type of this empty list from context");
                return t_poison();
            }
            if (fixed) {
                uint32_t length = static_cast<uint32_t>(std::stoul(expected->name));
                if (e->args.size() != length) {
                    error_at(e->line, e->col,
                             "fixed array literal needs " + std::to_string(length) +
                                 " element(s), got " + std::to_string(e->args.size()));
                }
                return expected;
            }
            return pool_.named(Type::K::class_, "List", {elem});
        }
        case Expr::Kind::init:
            return check_init(e, expected);
        case Expr::Kind::cast: {
            TypeId from = check_expr(e->object.get(), nullptr);
            TypeId to = resolve_type(e->type.get());
            if (from->k == Type::K::poison || to->k == Type::K::poison) {
                return e->checked ? t_option(to) : to;
            }
            if (e->checked) {
                bool ok = from->k == Type::K::class_ && to->k == Type::K::class_ &&
                          from->args.empty() && to->args.empty() &&
                          is_subclass_of(to->name, from->name) && to->name != from->name;
                if (!ok) {
                    error_at(e->line, e->col, "as? goes from a parent to a child class — " +
                                                  type_name(from) + " as? " + type_name(to) +
                                                  " doesn't");
                }
                return t_option(to);
            }
            bool numeric = from->is_numeric() && to->is_numeric();
            bool upcast = assignable(from, to);
            if (!numeric && !upcast) {
                error_at(e->line, e->col,
                         "can't cast " + type_name(from) + " as " + type_name(to));
            }
            return to;
        }
        case Expr::Kind::try_: {
            TypeId t = check_expr(e->object.get(), nullptr);
            if (t->k == Type::K::poison) return t;
            if (t->k == Type::K::enum_ && t->name == "Result" && t->args.size() == 2) {
                if (!cur_ret_ || cur_ret_->k != Type::K::enum_ || cur_ret_->name != "Result") {
                    error_at(e->line, e->col,
                             "? passes the error up, so this function must return Result");
                } else if (cur_ret_->args[1] != t->args[1]) {
                    error_at(e->line, e->col, "? error types differ: " + type_name(t->args[1]) +
                                                  " vs " + type_name(cur_ret_->args[1]));
                }
                if (is_move_only(t->args[0])) require_move_source(e->object.get(), t, "?");
                return t->args[0];
            }
            if (t->k == Type::K::enum_ && t->name == "Option" && t->args.size() == 1) {
                if (!cur_ret_ || cur_ret_->k != Type::K::enum_ || cur_ret_->name != "Option") {
                    error_at(e->line, e->col,
                             "? on an Option needs the function to return Option");
                }
                if (is_move_only(t->args[0])) require_move_source(e->object.get(), t, "?");
                return t->args[0];
            }
            error_at(e->line, e->col, "? works on Result or Option, got " + type_name(t));
            return t_poison();
        }
        case Expr::Kind::closure: {
            std::vector<TypeId> params;
            for (const Param& p : e->params) {
                if (p.passing == Param::Passing::move) {
                    error_at(p.line, p.col,
                             "move parameters are not supported on closure values yet");
                } else if (p.passing == Param::Passing::inout) {
                    error_at(p.line, p.col,
                             "inout parameters are not supported on closure values yet");
                }
                params.push_back(resolve_type(p.type.get()));
            }
            TypeId ret = e->type ? resolve_type(e->type.get()) : t_unit();

            TypeId saved_ret = cur_ret_;
            cur_ret_ = ret;
            int saved_take_floor = take_floor_depth_;
            int saved_capture_floor = capture_floor_depth_;
            take_floor_depth_ = static_cast<int>(scopes_.size());
            capture_floor_depth_ = static_cast<int>(scopes_.size());
            push_scope();
            for (size_t i = 0; i < e->params.size(); i++) {
                declare(e->params[i].name, params[i], false,
                        e->params[i].line, e->params[i].col, true);
            }
            int saved_depth = block_depth_;
            block_depth_ = 0; // the closure body is its own frame for defer
            check_block(e->body);
            block_depth_ = saved_depth;
            pop_scope();
            take_floor_depth_ = saved_take_floor;
            capture_floor_depth_ = saved_capture_floor;
            cur_ret_ = saved_ret;
            if (e->type && ret != t_unit() && ret->k != Type::K::poison &&
                !always_returns(e->body)) {
                error_at(e->line, e->col, "this closure must return " + type_name(ret) +
                                              " — the body can finish without a return");
            }
            return pool_.fn(std::move(params), ret);
        }
        case Expr::Kind::if_expr: {
            TypeId c = check_expr(e->cond.get(), t_bool());
            if (c != t_bool() && c->k != Type::K::poison) {
                error_at(e->cond->line, e->cond->col,
                         "condition must be bool, got " + type_name(c));
            }
            auto base = scopes_;
            TypeId a = check_expr(e->then_e.get(), expected);
            auto yes = scopes_;
            scopes_ = base;
            TypeId b = check_expr(e->else_e.get(), expected ? expected : a);
            auto no = scopes_;
            scopes_ = base;
            merge_move_states(yes, no);
            if (assignable(b, a)) return a;
            if (assignable(a, b)) return b;
            error_at(e->line, e->col, "if branches disagree: " + type_name(a) + " vs " +
                                          type_name(b));
            return t_poison();
        }
        case Expr::Kind::match_expr:
            return check_match(e, expected);
    }
    return t_poison();
}

TypeId Checker::check_binary(const Expr* e) {
    TokenKind op = e->op;

    if (op == TokenKind::andand || op == TokenKind::oror) {
        TypeId l = check_expr(e->lhs.get(), t_bool());
        TypeId r = check_expr(e->rhs.get(), t_bool());
        if ((l != t_bool() && l->k != Type::K::poison) ||
            (r != t_bool() && r->k != Type::K::poison)) {
            error_at(e->line, e->col, std::string(to_string(op)) + " needs bools");
        }
        return t_bool();
    }

    TypeId l = check_expr(e->lhs.get(), nullptr);
    TypeId r = check_expr(e->rhs.get(), l);
    if (l != r && is_adaptable_literal(e->lhs.get())) {
        l = check_expr(e->lhs.get(), r);
    }
    if (l->k == Type::K::poison || r->k == Type::K::poison) return t_poison();

    bool has_simd = (l->k == Type::K::class_ && l->name == "Simd4f32") ||
                    (r->k == Type::K::class_ && r->name == "Simd4f32");
    if (has_simd) {
        if (unsafe_depth_ == 0)
            error_at(e->line, e->col, "Simd4f32 arithmetic requires unsafe { }");
        if (l == r && (op == TokenKind::plus || op == TokenKind::minus ||
                       op == TokenKind::star || op == TokenKind::slash)) {
            return l;
        }
        error_at(e->line, e->col,
                 "Simd4f32 only supports +, -, *, and / with another Simd4f32");
        return t_poison();
    }

    switch (op) {
        case TokenKind::eq:
        case TokenKind::neq: {
            bool comparable = l == r && trait_satisfied(l, "Eq");
            if (!comparable) {
                error_at(e->line, e->col, "can't compare " + type_name(l) + " with " +
                                              type_name(r));
            }
            return t_bool();
        }
        case TokenKind::lt:
        case TokenKind::le:
        case TokenKind::gt:
        case TokenKind::ge: {
            bool ok = l == r && trait_satisfied(l, "Order");
            if (!ok) {
                error_at(e->line, e->col, "can't order " + type_name(l) + " and " +
                                              type_name(r));
            }
            return t_bool();
        }
        case TokenKind::plus:
            if (l->k == Type::K::string_ || r->k == Type::K::string_) {
                error_at(e->line, e->col,
                         "no + on strings — use interpolation: \"{a}{b}\"");
                return t_str();
            }
            [[fallthrough]];
        case TokenKind::minus:
        case TokenKind::star:
        case TokenKind::slash: {
            if (!(l == r && l->is_numeric())) {
                error_at(e->line, e->col,
                         std::string(to_string(op)) + " needs the same number type on both sides, got " +
                             type_name(l) + " and " + type_name(r) +
                             (l->is_numeric() && r->is_numeric() ? " — beans never converts numbers silently, use as" : ""));
                return t_poison();
            }
            return l;
        }
        case TokenKind::percent:
        case TokenKind::shl:
        case TokenKind::shr:
        case TokenKind::amp:
        case TokenKind::pipe:
        case TokenKind::caret: {
            if (!(l == r && l->is_int())) {
                error_at(e->line, e->col, std::string(to_string(op)) +
                                              " needs the same integer type on both sides, got " +
                                              type_name(l) + " and " + type_name(r));
                return t_poison();
            }
            return l;
        }
        default:
            error_at(e->line, e->col, "unsupported operator");
            return t_poison();
    }
}

// member lookup on an instance ------------------------------------------------

Checker::Member Checker::lookup_member(TypeId recv, const std::string& name,
                                       uint32_t line, uint32_t col) {
    Member none;
    if (!recv) return none;

    if (recv->k == Type::K::type_param) {
        auto bounds = cur_type_bounds_.find(recv->name);
        if (bounds == cur_type_bounds_.end()) return none;
        for (const std::string& bound : bounds->second) {
            auto it = classes_.find(bound);
            if (it == classes_.end() || !it->second.decl ||
                !it->second.decl->is_interface)
                continue;
            TypeId iface = pool_.named(Type::K::class_, bound);
            Member member = lookup_member(iface, name, line, col);
            if (member.kind != Member::Kind::none) return member;
        }
        return none;
    }

    if (recv->k == Type::K::class_ &&
        (recv->name == "RawPtr" || recv->name == "Slice" ||
         recv->name == "Simd4f32")) {
        Member builtin = builtin_member(recv, name);
        if (builtin.kind != Member::Kind::none && unsafe_depth_ == 0) {
            error_at(line, col, recv->name + "." + name + " requires unsafe { }");
        }
        return builtin;
    }

    if (recv->k == Type::K::class_ || recv->k == Type::K::struct_) {
        auto it = classes_.find(recv->name);
        if (it != classes_.end()) {
            if (it->second.decl && it->second.decl->is_union && unsafe_depth_ == 0)
                error_at(line, col, "union field access requires unsafe { }");
            // user class: walk the chain
            std::map<std::string, TypeId> env;
            const ClassInfo* info = &it->second;
            for (size_t i = 0; i < info->generic_params.size() && i < recv->args.size(); i++) {
                env[info->generic_params[i]] = recv->args[i];
            }
            std::set<std::string> seen;
            std::vector<const ClassInfo*> work = {info};
            bool first = true;
            while (!work.empty()) {
                const ClassInfo* c = work.back();
                work.pop_back();
                if (!seen.insert(c->decl->qualname).second) continue;
                auto fit = c->fields.find(name);
                if (fit != c->fields.end()) {
                    check_pub(c->decl->qualname, c->field_decls.at(name)->is_pub,
                              line, col, "field", recv->name + "." + name);
                    Member m;
                    m.kind = Member::Kind::field;
                    m.type = first ? subst(fit->second, env) : fit->second;
                    return m;
                }
                auto mit = c->methods.find(name);
                if (mit != c->methods.end() &&
                    (first || c->method_decls.at(name)->has_self)) {
                    // a pub interface's methods travel with it — the interface
                    // IS its method set, hiding them would make it useless
                    bool open = c->method_decls.at(name)->is_pub ||
                                (c->decl->is_interface && c->decl->is_pub);
                    check_pub(c->decl->qualname, open, line, col, "method",
                              recv->name + "." + name);
                    Member m;
                    m.kind = Member::Kind::method;
                    m.type = first ? subst(mit->second, env) : mit->second;
                    m.decl = c->method_decls.at(name);
                    m.is_static = !m.decl->has_self;
                    return m;
                }
                for (const std::string& s : c->supers) {
                    auto sit = classes_.find(s);
                    if (sit != classes_.end()) work.push_back(&sit->second);
                }
                first = false;
            }
            return builtin_member(recv, name);
        }
        return builtin_member(recv, name);
    }

    if (recv->k == Type::K::enum_) {
        auto it = enums_.find(recv->name);
        if (it != enums_.end() && it->second.decl) {
            std::map<std::string, TypeId> env;
            for (size_t i = 0; i < it->second.generic_params.size() && i < recv->args.size(); i++) {
                env[it->second.generic_params[i]] = recv->args[i];
            }
            auto mit = it->second.methods.find(name);
            if (mit != it->second.methods.end()) {
                check_pub(it->second.decl->qualname, it->second.method_decls.at(name)->is_pub,
                          line, col, "method", recv->name + "." + name);
                Member m;
                m.kind = Member::Kind::method;
                m.type = subst(mit->second, env);
                m.decl = it->second.method_decls.at(name);
                m.is_static = !m.decl->has_self;
                return m;
            }
        }
        return builtin_member(recv, name);
    }

    return builtin_member(recv, name);
}

Checker::Member Checker::builtin_member(TypeId recv, const std::string& name) {
    Member none;
    auto method = [&](std::vector<TypeId> params, TypeId ret) {
        Member m;
        m.kind = Member::Kind::method;
        m.type = pool_.fn(std::move(params), ret);
        return m;
    };
    auto field = [&](TypeId t) {
        Member m;
        m.kind = Member::Kind::field;
        m.type = t;
        return m;
    };

    if (recv->is_int()) {
        if (name == "abs") return method({}, recv);
        return none;
    }
    if (recv->is_float()) {
        if (name == "abs") return method({}, recv);
        if (name == "round") return method({}, t_int());
        return none;
    }
    if (recv->k == Type::K::decimal_) {
        if (name == "abs") return method({}, recv);
        if (name == "round") return method({t_int()}, recv);
        return none;
    }
    if (recv->k == Type::K::fixed_array) {
        if (name == "len") return method({}, t_int());
        return none;
    }
    // string, Bytes, File, and MMap methods come from the registry
    if (recv->k == Type::K::string_ ||
        (recv->k == Type::K::class_ &&
         (recv->name == "Bytes" || recv->name == "File" || recv->name == "MMap"))) {
        BT want = recv->k == Type::K::string_  ? BT::str
                  : recv->name == "Bytes"      ? BT::bytes
                  : recv->name == "MMap"       ? BT::mmap
                                               : BT::file;
        for (const BuiltinMethod& b : builtin_methods()) {
            if (b.recv == want && name == b.name) {
                std::vector<TypeId> ps;
                for (BT p : b.params) ps.push_back(bt_type(p, recv));
                return method(std::move(ps), bt_type(b.ret, recv));
            }
        }
        return none;
    }
    if (recv->k == Type::K::class_) {
        if (recv->name == "RawPtr" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "read") return method({}, T);
            if (name == "write") return method({T}, t_unit());
            if (name == "read_volatile") return method({}, T);
            if (name == "write_volatile") return method({T}, t_unit());
            if (name == "offset") return method({t_int()}, recv);
            if (name == "address") return method({}, pool_.prim(Type::K::u64_));
            if (name == "is_null") return method({}, t_bool());
            if (name == "element_size" || name == "element_align")
                return method({}, t_int());
            if (name == "copy_from") return method({recv, t_int()}, t_unit());
            if (name == "fill_zero") return method({t_int()}, t_unit());
            if (name == "free") return method({}, t_unit());
            if ((T->is_int() || T->k == Type::K::bool_) && name == "atomic_load")
                return method({}, T);
            if ((T->is_int() || T->k == Type::K::bool_) && name == "atomic_store")
                return method({T}, t_unit());
            if ((T->is_int() || T->k == Type::K::bool_) &&
                name == "atomic_compare_exchange")
                return method({T, T}, t_bool());
            if (T->is_int() && name == "atomic_fetch_add") return method({T}, T);
            return none;
        }
        if (recv->name == "Slice" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "len") return method({}, t_int());
            if (name == "get") return method({t_int()}, T);
            if (name == "set") return method({t_int(), T}, t_unit());
            if (name == "subslice") return method({t_int(), t_int()}, recv);
            if (name == "as_ptr")
                return method({}, pool_.named(Type::K::class_, "RawPtr", {T}));
            return none;
        }
        if (recv->name == "Box" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "get" && !is_move_only(T)) return method({}, T);
            if (name == "set") return method({T}, t_unit());
            return none;
        }
        if (recv->name == "Arena" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "put") return method({T}, t_int());
            if (name == "get" && !is_move_only(T)) return method({t_int()}, t_option(T));
            if (name == "at" && !is_move_only(T)) return method({t_int()}, T);
            if (name == "len") return method({}, t_int());
            if (name == "clear") return method({}, t_unit());
            return none;
        }
        if (recv->name == "Shared" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "get" && !is_move_only(T)) return method({}, T);
            if (name == "downgrade")
                return method({}, pool_.named(Type::K::class_, "Weak", {T}));
            return none;
        }
        if (recv->name == "Weak" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "upgrade")
                return method({}, t_option(pool_.named(Type::K::class_, "Shared", {T})));
            if (name == "expired") return method({}, t_bool());
            return none;
        }
        if (recv->name == "List" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            bool wide = is_wide_inline_value(T);
            bool ordered = trait_satisfied(T, "Order");
            bool equatable = trait_satisfied(T, "Eq");
            if (name == "push") return method({T}, t_unit());
            if (name == "clone" && !is_move_only(T) && trait_satisfied(T, "Clone"))
                return method({}, recv);
            if (name == "reserve") return method({t_int()}, t_unit());
            if (name == "pop") return method({}, t_option(T));
            if (name == "get" && !is_move_only(T)) return method({t_int()}, t_option(T));
            if ((name == "first" || name == "last") && !is_move_only(T))
                return method({}, t_option(T));
            if (name == "len") return method({}, t_int());
            if (!wide && (name == "max" || name == "min"))
                return ordered ? method({}, t_option(T)) : none;
            if (!wide && name == "contains" && equatable) return method({T}, t_bool());
            if (!wide && name == "index_of" && equatable)
                return method({T}, t_option(t_int()));
            if (name == "insert") return method({t_int(), T}, t_unit());
            if (name == "remove") return method({t_int()}, T);
            if (name == "reverse" || name == "clear") return method({}, t_unit());
            if (name == "slice") return method({t_int(), t_int()}, recv);
            if (!wide && name == "sort") return ordered ? method({}, t_unit()) : none;
            if (!wide && name == "sort_by")
                return method({pool_.fn({T, T}, t_bool())}, t_unit());
            if (!wide && name == "sort_by_key")
                return method({pool_.fn({T}, t_int())}, t_unit());
            if (!wide && name == "join") return method({t_str()}, t_str());
            return none;
        }
        if ((recv->name == "Map" || recv->name == "OrderedMap") &&
            recv->args.size() == 2) {
            TypeId K = recv->args[0], V = recv->args[1];
            if (name == "get" && !is_move_only(V)) return method({K}, t_option(V));
            if (name == "set") return method({K, V}, t_unit());
            if (name == "insert") return method({K, V}, t_bool());
            if (name == "clone" && !is_move_only(K) && !is_move_only(V) &&
                trait_satisfied(K, "Clone") && trait_satisfied(V, "Clone"))
                return method({}, recv);
            if (name == "reserve") return method({t_int()}, t_unit());
            if (name == "len") return method({}, t_int());
            if (name == "contains") return method({K}, t_bool());
            if (name == "remove") return method({K}, t_bool());
            if (name == "keys")
                return method({}, pool_.named(Type::K::class_, "List", {K}));
            if (name == "values" && !is_move_only(V))
                return method({}, pool_.named(Type::K::class_, "List", {V}));
            if (name == "clear") return method({}, t_unit());
            return none;
        }
        if (recv->name == "Thread" && recv->args.size() == 1) {
            if (name == "join") return method({}, recv->args[0]);
            return none;
        }
        if (recv->name == "Mutex" && recv->args.size() == 1) {
            if (name == "with")
                return method({pool_.fn({recv->args[0]}, t_unit())}, t_unit());
            return none;
        }
        if (recv->name == "Channel" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "send") return method({T}, t_unit());
            if (name == "recv") return method({}, t_option(T));
            if (name == "close") return method({}, t_unit());
            return none;
        }
        if (recv->name == "AtomicInt") {
            if (name == "add") return method({t_int()}, t_int());
            if (name == "get") return method({}, t_int());
            if (name == "set") return method({t_int()}, t_unit());
            return none;
        }
        if (recv->name == "Simd4f32") {
            if (name == "lane") return method({t_int()}, pool_.prim(Type::K::f32));
            if (name == "sum") return method({}, pool_.prim(Type::K::f32));
            if (name == "store") {
                TypeId f32 = pool_.prim(Type::K::f32);
                return method({pool_.named(Type::K::class_, "RawPtr", {f32})}, t_unit());
            }
            return none;
        }
        if (recv->name == "Error") {
            if (name == "msg" || name == "kind") return field(t_str());
            return none;
        }
        return none;
    }
    if (recv->k == Type::K::enum_) {
        if (recv->name == "Option" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "or") return method({T}, T);
            if (name == "expect") return method({t_str()}, T);
            if (name == "is_some" || name == "is_none") return method({}, t_bool());
            if (name == "map" || name == "and_then" || name == "filter")
                return method({}, t_poison());
            return none;
        }
        if (recv->name == "Result" && recv->args.size() == 2) {
            TypeId T = recv->args[0];
            if (name == "or") return method({T}, T);
            if (name == "expect") return method({t_str()}, T);
            if (name == "is_ok") return method({}, t_bool());
            if (name == "map" || name == "and_then" || name == "recover")
                return method({}, t_poison());
            return none;
        }
        return none;
    }
    return none;
}

// field access (not a call) ---------------------------------------------------

TypeId Checker::check_field(const Expr* e, bool /*for_call*/, TypeId* /*self_type_out*/) {
    const Expr* obj = e->object.get();

    // the whole expression names a type (`util.User` as a value)?
    if (std::string self_tn = as_type_name(e); !self_tn.empty()) {
        error_at(e->line, e->col, "'" + self_tn + "' is a type, not a value");
        return t_poison();
    }

    // Type-name and package receivers only make sense in calls; handle values here.
    std::string tn = as_type_name(obj);
    if (!tn.empty()) {
        if (auto eit = enums_.find(tn); eit != enums_.end()) {
            // bare variant without payload as a value: Status.active
            auto vit = eit->second.variants.find(e->name);
            if (vit != eit->second.variants.end()) {
                if (!vit->second.empty()) {
                    error_at(e->line, e->col, "variant '" + e->name + "' carries data — call it");
                    return t_poison();
                }
                if (!eit->second.generic_params.empty()) {
                    error_at(e->line, e->col, "can't use a generic enum variant bare here");
                    return t_poison();
                }
                return pool_.named(Type::K::enum_, tn);
            }
            error_at(e->line, e->col, "enum " + tn + " has no variant '" + e->name + "'");
            return t_poison();
        }
        error_at(e->line, e->col,
                 "'" + tn + "." + e->name + "' is a static — call it with (...)");
        return t_poison();
    }
    if (obj->kind == Expr::Kind::ident && !find_local(std::string(obj->text)) &&
        pkg_paths_.count(std::string(obj->text))) {
        error_at(e->line, e->col, "package members must be called: " +
                                      std::string(obj->text) + "." + e->name + "(...)");
        return t_poison();
    }

    TypeId t = check_expr(obj, nullptr);
    if (t->k == Type::K::poison) return t;
    Member m = lookup_member(t, e->name, e->line, e->col);
    if (m.kind == Member::Kind::field) return m.type;
    if (m.kind == Member::Kind::method) {
        error_at(e->line, e->col, "'" + e->name + "' is a method — call it with (...)");
        return t_poison();
    }
    error_at(e->line, e->col, type_name(t) + " has no field or method '" + e->name + "'");
    return t_poison();
}

// calls -----------------------------------------------------------------------

TypeId Checker::check_call(const Expr* e, TypeId expected) {
    const Expr* callee = e->callee.get();

    auto check_args_against = [&](const std::vector<TypeId>& params, TypeId ret,
                                  const std::string& what,
                                  const std::vector<Param>* declared = nullptr) -> TypeId {
        if (e->args.size() != params.size()) {
            error_at(e->line, e->col, what + " takes " + std::to_string(params.size()) +
                                          " argument(s), got " + std::to_string(e->args.size()));
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
            return ret;
        }
        for (size_t i = 0; i < e->args.size(); i++) {
            bool saved_inout = allow_inout_expr_;
            allow_inout_expr_ = declared && i < declared->size() &&
                                (*declared)[i].passing == Param::Passing::inout;
            TypeId got = check_expr(e->args[i].get(), params[i]);
            allow_inout_expr_ = saved_inout;
            if (!assignable(got, params[i])) {
                error_at(e->args[i]->line, e->args[i]->col,
                         what + " argument " + std::to_string(i + 1) + " is " +
                             type_name(params[i]) + ", got " + type_name(got));
            }
        }
        return ret;
    };

    auto check_passing = [&](const std::vector<Param>& declared,
                             const std::vector<TypeId>& types,
                             const std::string& what) {
        std::set<std::string> inout_names;
        for (size_t i = 0; i < e->args.size() && i < declared.size() && i < types.size(); i++) {
            if (declared[i].passing == Param::Passing::move) {
                require_move_source(e->args[i].get(), types[i],
                                    what + " move argument " + std::to_string(i + 1));
            } else if (declared[i].passing == Param::Passing::inout) {
                const Expr* arg = e->args[i].get();
                if (!arg || arg->kind != Expr::Kind::unary ||
                    arg->op != TokenKind::kw_inout || !arg->rhs ||
                    arg->rhs->kind != Expr::Kind::ident) {
                    error_at(arg ? arg->line : e->line, arg ? arg->col : e->col,
                             what + " inout argument " + std::to_string(i + 1) +
                                 " must be 'inout var_name'");
                } else {
                    std::string name(arg->rhs->text);
                    if (!inout_names.insert(name).second) {
                        error_at(arg->line, arg->col,
                                 "overlapping inout arguments for '" + name + "'");
                    }
                }
            }
        }
    };

    // generic top-level fn: infer type params from the arguments
    auto call_generic_fn = [&](const FnDecl* decl, TypeId fn_t,
                               const std::string& what) -> TypeId {
        if (decl->is_extern_c && unsafe_depth_ == 0) {
            error_at(e->line, e->col,
                     "extern C call '" + decl->name + "' requires unsafe { }");
        }
        if (decl->generics.empty()) {
            TypeId ret = check_args_against(fn_t->fn_params, fn_t->fn_ret, what,
                                            &decl->params);
            check_passing(decl->params, fn_t->fn_params, what);
            return ret;
        }
        if (e->args.size() != fn_t->fn_params.size()) {
            error_at(e->line, e->col, what + " takes " +
                                          std::to_string(fn_t->fn_params.size()) +
                                          " argument(s), got " + std::to_string(e->args.size()));
            return t_poison();
        }
        std::map<std::string, TypeId> env;
        std::vector<TypeId> arg_types;
        for (size_t i = 0; i < e->args.size(); i++) {
            bool saved_inout = allow_inout_expr_;
            allow_inout_expr_ = i < decl->params.size() &&
                                decl->params[i].passing == Param::Passing::inout;
            TypeId got = check_expr(e->args[i].get(), nullptr);
            allow_inout_expr_ = saved_inout;
            arg_types.push_back(got);
            unify(fn_t->fn_params[i], got, env);
        }
        for (const GenericParam& g : decl->generics) {
            if (!env.count(g.name)) {
                error_at(e->line, e->col,
                         "can't work out type parameter '" + g.name + "' for " + what);
                env[g.name] = t_poison();
            }
        }
        check_generic_bounds(decl->generics, env, e->line, e->col, what);
        for (size_t i = 0; i < e->args.size(); i++) {
            TypeId want = subst(fn_t->fn_params[i], env);
            if (!assignable(arg_types[i], want)) {
                error_at(e->args[i]->line, e->args[i]->col,
                         what + " argument " + std::to_string(i + 1) + " is " +
                             type_name(want) + ", got " + type_name(arg_types[i]));
            }
        }
        std::vector<TypeId> concrete_params;
        for (TypeId param : fn_t->fn_params) concrete_params.push_back(subst(param, env));
        check_passing(decl->params, concrete_params, what);
        TypeId concrete_ret = subst(fn_t->fn_ret, env);
        return concrete_ret;
    };

    // ---- callee is a plain name ----
    if (callee->kind == Expr::Kind::ident) {
        std::string name(callee->text);

        if (const Local* l = find_local(name)) {
            if (l->type->k == Type::K::fn) {
                return check_args_against(l->type->fn_params, l->type->fn_ret, "'" + name + "'");
            }
            if (l->type->k != Type::K::poison) {
                error_at(e->line, e->col, "'" + name + "' is not a function");
            }
            return t_poison();
        }

        if (name == "some") {
            if (e->args.size() != 1) {
                error_at(e->line, e->col, "some takes 1 argument");
                return t_poison();
            }
            TypeId inner_expected = nullptr;
            if (expected && expected->k == Type::K::enum_ && expected->name == "Option")
                inner_expected = expected->args[0];
            TypeId got = check_expr(e->args[0].get(), inner_expected);
            require_move_source(e->args[0].get(), got, "some");
            if (inner_expected && !assignable(got, inner_expected)) {
                error_at(e->line, e->col, "some(...) here needs " + type_name(inner_expected) +
                                              ", got " + type_name(got));
            }
            return t_option(inner_expected ? inner_expected : got);
        }
        if (name == "ok" || name == "err") {
            if (e->args.size() != 1) {
                error_at(e->line, e->col, name + " takes 1 argument");
                return t_poison();
            }
            TypeId ok_t = nullptr, err_t = t_error_class();
            if (expected && expected->k == Type::K::enum_ && expected->name == "Result") {
                ok_t = expected->args[0];
                err_t = expected->args[1];
            }
            if (name == "ok") {
                TypeId got = check_expr(e->args[0].get(), ok_t);
                require_move_source(e->args[0].get(), got, "ok");
                if (ok_t && !assignable(got, ok_t)) {
                    error_at(e->line, e->col, "ok(...) here needs " + type_name(ok_t) +
                                                  ", got " + type_name(got));
                }
                return t_result(ok_t ? ok_t : got, err_t);
            }
            TypeId got = check_expr(e->args[0].get(),
                                    err_t == t_error_class() ? nullptr : err_t);
            require_move_source(e->args[0].get(), got, "err");
            bool wraps_string = err_t == t_error_class() && got->k == Type::K::string_;
            if (!wraps_string && !assignable(got, err_t)) {
                error_at(e->line, e->col, "err(...) here needs " + type_name(err_t) +
                                              (err_t == t_error_class() ? " or a string message" : "") +
                                              ", got " + type_name(got));
            }
            if (!ok_t) {
                error_at(e->line, e->col,
                         "can't tell the ok-type of this err(...) — the spot needs a declared Result type");
                ok_t = t_poison();
            }
            return t_result(ok_t, err_t);
        }

        std::string fkey = resolve_fn_key(name);
        if (!fkey.empty()) {
            callee->resolved = fkey;
            return call_generic_fn(top_fn_decls_[fkey], top_fns_[fkey], "'" + name + "'");
        }

        // Classes are never callable; construction is explicit.
        std::string ckey = resolve_class_key(name, e->line, e->col);
        if (!ckey.empty()) {
            error_at(e->line, e->col, "classes are built with 'new " + name + "(...)'");
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
            return t_poison();
        }

        error_at(e->line, e->col, "unknown function '" + name + "'");
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
        return t_poison();
    }

    // ---- callee is a field access ----
    if (callee->kind == Expr::Kind::field) {
        const Expr* obj = callee->object.get();
        const std::string& mname = callee->name;

        if (obj->kind == Expr::Kind::ident && !find_local(std::string(obj->text))) {
            std::string n(obj->text);

            // super.init(...) — contextual, not a keyword: only this exact
            // form, only inside an init whose ancestor declares one
            if (n == "super") {
                if (mname != "init") {
                    error_at(e->line, e->col,
                             "only super.init(...) exists for now — call parent "
                             "methods on self");
                    for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                    return t_poison();
                }
                return check_super_init(e);
            }

            // package call
            auto pit = pkg_paths_.find(n);
            if (pit != pkg_paths_.end()) {
                const std::string& path = pit->second;
                if (path == "std.io" && (mname == "println" || mname == "print" ||
                                         mname == "eprintln" || mname == "eprint")) {
                    callee->resolved = "std.io." + mname;
                    if (e->args.size() != 1) {
                        error_at(e->line, e->col, "io." + mname + " takes 1 argument");
                    }
                    for (const ExprPtr& a : e->args) {
                        TypeId t = check_expr(a.get(), nullptr);
                        if (!printable(t)) {
                            error_at(a->line, a->col, "can't print " + type_name(t) +
                                                          " yet — give it a string form first");
                        }
                    }
                    return t_unit();
                }
                // table-driven std functions (std.os, io.read_line, ...)
                for (const BuiltinFn& b : builtin_fns()) {
                    if (path == b.module && mname == b.name) {
                        callee->resolved = std::string(b.module) + "." + b.name;
                        std::vector<TypeId> ps;
                        for (BT p : b.params) ps.push_back(bt_type(p, nullptr));
                        return check_args_against(ps, bt_type(b.ret, nullptr),
                                                  "'" + n + "." + mname + "'");
                    }
                }
                if (path == "std.thread" && mname == "spawn") {
                    callee->resolved = "std.thread.spawn";
                    if (e->args.size() != 1) {
                        error_at(e->line, e->col, "thread.spawn takes 1 closure");
                        return t_poison();
                    }
                    bool saved_send = require_send_captures_;
                    std::set<std::string> saved_bad = std::move(bad_send_captures_);
                    require_send_captures_ = true;
                    bad_send_captures_.clear();
                    TypeId f = check_expr(e->args[0].get(), nullptr);
                    require_send_captures_ = saved_send;
                    bad_send_captures_ = std::move(saved_bad);
                    if (f->k != Type::K::fn || !f->fn_params.empty()) {
                        error_at(e->line, e->col,
                                 "thread.spawn needs a closure with no parameters");
                        return t_poison();
                    }
                    if (!trait_satisfied(f->fn_ret, "Send")) {
                        error_at(e->line, e->col,
                                 "thread.spawn closure returns non-Send type " +
                                     type_name(f->fn_ret));
                    }
                    return pool_.named(Type::K::class_, "Thread", {f->fn_ret});
                }
                // a real package: pub fn call across the package line
                auto pfx = prefix_by_path_.find(path);
                if (pfx != prefix_by_path_.end()) {
                    std::string key = pfx->second + "." + mname;
                    auto fit = top_fns_.find(key);
                    if (fit != top_fns_.end()) {
                        check_pub(key, top_fn_decls_[key]->is_pub, e->line, e->col,
                                  "function", n + "." + mname);
                        callee->resolved = key;
                        return call_generic_fn(top_fn_decls_[key], fit->second,
                                               "'" + n + "." + mname + "'");
                    }
                    // A package-qualified class is still not a function.
                    auto cit2 = classes_.find(key);
                    if (cit2 != classes_.end()) {
                        check_pub(key, cit2->second.decl && cit2->second.decl->is_pub,
                                  e->line, e->col, "type", n + "." + mname);
                        error_at(e->line, e->col, "classes are built with 'new " +
                                                      n + "." + mname + "(...)'");
                        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                        return t_poison();
                    }
                }
                error_at(e->line, e->col,
                         "package '" + n + "' (" + path + ") has no function '" + mname + "'");
                for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                return t_poison();
            }
        }

        // type-name receiver: enum variant construction or a static call,
        // local (`Payment.transfer`) or imported (`util.Payment.transfer`)
        std::string tn = as_type_name(obj);
        if (!tn.empty()) {
            std::string n = tn;

            auto eit = enums_.find(n);
            if (eit != enums_.end()) {
                auto vit = eit->second.variants.find(mname);
                if (vit == eit->second.variants.end()) {
                    // maybe a static method on the enum? fall through to error
                    error_at(e->line, e->col, "enum " + n + " has no variant '" + mname + "'");
                    return t_poison();
                }
                if (!eit->second.generic_params.empty()) {
                    if (!expected || expected->k != Type::K::enum_ ||
                        expected->name != n ||
                        expected->args.size() != eit->second.generic_params.size()) {
                        error_at(e->line, e->col,
                                 "can't infer generic enum '" + n +
                                     "' — declare the result type");
                        return t_poison();
                    }
                    std::map<std::string, TypeId> env;
                    for (size_t i = 0; i < eit->second.generic_params.size(); i++)
                        env[eit->second.generic_params[i]] = expected->args[i];
                    std::vector<TypeId> payload;
                    for (TypeId type : vit->second) payload.push_back(subst(type, env));
                    return check_args_against(payload, expected, n + "." + mname);
                }
                return check_args_against(vit->second,
                                          pool_.named(Type::K::enum_, n),
                                          n + "." + mname);
            }

            // builtin statics
            if (n == "RawPtr") {
                if (unsafe_depth_ == 0) {
                    error_at(e->line, e->col,
                             "RawPtr." + mname + " requires unsafe { }");
                }
                TypeId result = nullptr;
                if (expected && expected->k == Type::K::class_ &&
                    expected->name == "RawPtr" && expected->args.size() == 1) {
                    result = expected;
                }
                if (!result) {
                    error_at(e->line, e->col,
                             "can't tell the pointer value type — declare it: "
                             "let ptr: RawPtr<T> = RawPtr." + mname + "(...)");
                    for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                    return t_poison();
                }
                if (mname == "alloc") {
                    if (e->args.size() != 1)
                        error_at(e->line, e->col, "RawPtr.alloc takes an element count");
                    for (const ExprPtr& a : e->args) check_expr(a.get(), t_int());
                    return result;
                }
                if (mname == "from_address") {
                    if (e->args.size() != 1)
                        error_at(e->line, e->col, "RawPtr.from_address takes a u64 address");
                    for (const ExprPtr& a : e->args)
                        check_expr(a.get(), pool_.prim(Type::K::u64_));
                    return result;
                }
                if (mname == "null") {
                    if (!e->args.empty())
                        error_at(e->line, e->col, "RawPtr.null takes no arguments");
                    for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                    return result;
                }
                error_at(e->line, e->col, "RawPtr has no static '" + mname + "'");
                for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                return t_poison();
            }
            if (n == "Simd4f32") {
                if (unsafe_depth_ == 0)
                    error_at(e->line, e->col,
                             "Simd4f32." + mname + " requires unsafe { }");
                TypeId f32 = pool_.prim(Type::K::f32);
                TypeId result = pool_.named(Type::K::class_, "Simd4f32");
                if (mname == "splat") {
                    return check_args_against({f32}, result, "'Simd4f32.splat'");
                }
                if (mname == "of") {
                    return check_args_against({f32, f32, f32, f32}, result,
                                              "'Simd4f32.of'");
                }
                if (mname == "load") {
                    TypeId pointer = pool_.named(Type::K::class_, "RawPtr", {f32});
                    return check_args_against({pointer}, result, "'Simd4f32.load'");
                }
                error_at(e->line, e->col, "Simd4f32 has no static '" + mname + "'");
                for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                return t_poison();
            }
            if (n == "Slice" && mname == "from_raw") {
                if (unsafe_depth_ == 0)
                    error_at(e->line, e->col,
                             "Slice.from_raw requires unsafe { }");
                TypeId result = nullptr;
                if (expected && expected->k == Type::K::class_ &&
                    expected->name == "Slice" && expected->args.size() == 1) {
                    result = expected;
                }
                if (!result) {
                    error_at(e->line, e->col,
                             "can't tell the slice element type — declare it: "
                             "let view: Slice<T> = Slice.from_raw(ptr, len)");
                    for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                    return t_poison();
                }
                TypeId pointer = pool_.named(Type::K::class_, "RawPtr",
                                            {result->args[0]});
                return check_args_against({pointer, t_int()}, result,
                                          "'Slice.from_raw'");
            }
            if (n == "Bytes" || n == "File" || n == "Dir" || n == "MMap") {
                for (const BuiltinStatic& b : builtin_statics()) {
                    if (n == b.cls && mname == b.name) {
                        std::vector<TypeId> ps;
                        for (BT p : b.params) ps.push_back(bt_type(p, nullptr));
                        return check_args_against(ps, bt_type(b.ret, nullptr),
                                                  "'" + n + "." + mname + "'");
                    }
                }
                error_at(e->line, e->col, n + " has no static '" + mname + "'");
                for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
                return t_poison();
            }

            // user class static
            auto cit = classes_.find(n);
            if (cit != classes_.end()) {
                auto mit = cit->second.methods.find(mname);
                if (mit == cit->second.methods.end()) {
                    error_at(e->line, e->col, n + " has no static '" + mname + "'");
                    return t_poison();
                }
                const FnDecl* md = cit->second.method_decls.at(mname);
                if (md->has_self) {
                    error_at(e->line, e->col,
                             "'" + mname + "' is an instance method — declare "
                             "'static fn " + mname + "' or call it on a " + n +
                                 " value");
                    return t_poison();
                }
                check_pub(n, md->is_pub, e->line, e->col, "static", n + "." + mname);
                if (!cit->second.generic_params.empty()) {
                    error_at(e->line, e->col,
                             "statics on generic classes aren't supported yet — use an initializer");
                    return t_poison();
                }
                TypeId ret = check_args_against(mit->second->fn_params,
                                                mit->second->fn_ret, n + "." + mname,
                                                &md->params);
                check_passing(md->params, mit->second->fn_params, n + "." + mname);
                return ret;
            }
        }

        // plain instance method call
        TypeId t = check_expr(obj, nullptr);
        if (t->k == Type::K::poison) {
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
            return t_poison();
        }
        if (t->k == Type::K::class_ && (mname == "init" || mname == "deinit")) {
            error_at(e->line, e->col,
                     mname == "init" ? "init runs when the object is built — use new " +
                                           type_name(t) + "(...)"
                                     : "deinit runs by itself when the last reference drops — "
                                       "never call it");
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
            return t_poison();
        }
        if (t->k == Type::K::enum_ &&
            (t->name == "Option" || t->name == "Result") &&
            (mname == "map" || mname == "and_then" || mname == "filter" ||
             mname == "recover")) {
            if (e->args.size() != 1) {
                error_at(e->line, e->col, type_name(t) + "." + mname +
                                              " takes one function");
                for (const ExprPtr& arg : e->args) check_expr(arg.get(), nullptr);
                return t_poison();
            }
            TypeId payload = t->args[0];
            if (!trait_satisfied(payload, "Clone")) {
                error_at(e->line, e->col, type_name(t) + "." + mname +
                                              " needs its value type to implement Clone");
            }
            TypeId function = check_expr(e->args[0].get(), nullptr);
            if (function->k != Type::K::fn || function->fn_params.size() != 1) {
                error_at(e->args[0]->line, e->args[0]->col,
                         type_name(t) + "." + mname + " needs a one-parameter function");
                return t_poison();
            }
            TypeId input = (mname == "recover" && t->name == "Result")
                               ? t->args[1]
                               : payload;
            if (!assignable(input, function->fn_params[0])) {
                error_at(e->args[0]->line, e->args[0]->col,
                         type_name(t) + "." + mname + " function takes " +
                             type_name(input) + ", got " +
                             type_name(function->fn_params[0]));
            }
            if (mname == "filter") {
                if (t->name != "Option") {
                    error_at(e->line, e->col, "filter is only available on Option");
                    return t_poison();
                }
                if (function->fn_ret != t_bool())
                    error_at(e->args[0]->line, e->args[0]->col,
                             "Option.filter function must return bool");
                return t;
            }
            if (mname == "recover") {
                if (t->name != "Result") {
                    error_at(e->line, e->col, "recover is only available on Result");
                    return t_poison();
                }
                if (!assignable(function->fn_ret, payload))
                    error_at(e->args[0]->line, e->args[0]->col,
                             "Result.recover function must return " + type_name(payload));
                return payload;
            }
            if (mname == "map") {
                return t->name == "Option"
                           ? t_option(function->fn_ret)
                           : t_result(function->fn_ret, t->args[1]);
            }
            if (t->name == "Option") {
                if (function->fn_ret->k != Type::K::enum_ ||
                    function->fn_ret->name != "Option") {
                    error_at(e->args[0]->line, e->args[0]->col,
                             "Option.and_then function must return Option");
                    return t_poison();
                }
                return function->fn_ret;
            }
            if (function->fn_ret->k != Type::K::enum_ ||
                function->fn_ret->name != "Result" ||
                function->fn_ret->args.size() != 2 ||
                function->fn_ret->args[1] != t->args[1]) {
                error_at(e->args[0]->line, e->args[0]->col,
                         "Result.and_then function must return Result with the same error type");
                return t_poison();
            }
            return function->fn_ret;
        }
        Member m = lookup_member(t, mname, e->line, e->col);
        if (m.kind == Member::Kind::method) {
            if (m.is_static) {
                error_at(e->line, e->col, "'" + mname + "' is a static — call it on the type name");
                return t_poison();
            }
            if (t->k == Type::K::class_ && t->name == "Box" && mname == "set" &&
                !e->args.empty()) {
                require_move_source(e->args[0].get(), m.type->fn_params[0], "Box.set");
            }
            if (t->k == Type::K::class_ && t->name == "Arena" && mname == "put" &&
                !e->args.empty()) {
                require_move_source(e->args[0].get(), m.type->fn_params[0], "Arena.put");
            }
            if (t->k == Type::K::class_ && t->name == "List") {
                if (mname == "push" && !e->args.empty())
                    require_move_source(e->args[0].get(), m.type->fn_params[0], "List.push");
                if (mname == "insert" && e->args.size() > 1)
                    require_move_source(e->args[1].get(), m.type->fn_params[1],
                                        "List.insert");
            }
            if (t->k == Type::K::class_ &&
                (t->name == "Map" || t->name == "OrderedMap") &&
                (mname == "set" || mname == "insert")) {
                for (size_t i = 0; i < e->args.size() && i < m.type->fn_params.size(); i++)
                    require_move_source(e->args[i].get(), m.type->fn_params[i],
                                        "Map." + mname);
            }
            if (t->k == Type::K::class_ && t->name == "Channel" && mname == "send" &&
                !e->args.empty()) {
                require_move_source(e->args[0].get(), m.type->fn_params[0], "Channel.send");
            }
            if (t->k == Type::K::enum_ &&
                (t->name == "Option" || t->name == "Result") &&
                (mname == "or" || mname == "expect") &&
                is_move_only(m.type->fn_ret)) {
                require_move_source(obj, t, type_name(t) + "." + mname);
                if (mname == "or" && !e->args.empty()) {
                    require_move_source(e->args[0].get(), m.type->fn_params[0],
                                        type_name(t) + ".or default");
                }
            }
            std::string shown = type_name(t) + "." + mname;
            TypeId ret = check_args_against(m.type->fn_params, m.type->fn_ret, shown,
                                            m.decl ? &m.decl->params : nullptr);
            if (m.decl) check_passing(m.decl->params, m.type->fn_params, shown);
            return ret;
        }
        if (m.kind == Member::Kind::field && m.type->k == Type::K::fn) {
            return check_args_against(m.type->fn_params, m.type->fn_ret,
                                      type_name(t) + "." + mname);
        }
        error_at(e->line, e->col, type_name(t) + " has no method '" + mname + "'");
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
        return t_poison();
    }

    // ---- anything else: closures returned from calls, etc. ----
    TypeId t = check_expr(callee, nullptr);
    if (t->k == Type::K::fn) {
        return check_args_against(t->fn_params, t->fn_ret, "this closure");
    }
    if (t->k != Type::K::poison) {
        error_at(e->line, e->col, "this " + type_name(t) + " isn't callable");
    }
    for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
    return t_poison();
}

// constructors ----------------------------------------------------------------

TypeId Checker::check_new(const Expr* e, TypeId expected) {
    auto explicit_args = [&]() {
        std::vector<TypeId> out;
        for (const TypePtr& arg : e->type_args) out.push_back(resolve_type(arg.get()));
        return out;
    };
    auto require_count = [&](size_t count, const std::string& shown) {
        if (e->args.size() == count) return true;
        error_at(e->line, e->col, shown + " takes " + std::to_string(count) +
                                      " argument(s), got " +
                                      std::to_string(e->args.size()));
        for (const ExprPtr& arg : e->args) check_expr(arg.get(), nullptr);
        return false;
    };

    const std::string& name = e->name;
    if (name == "Bytes") {
        if (!e->type_args.empty())
            error_at(e->line, e->col, "Bytes takes no type arguments");
        require_count(1, "new Bytes");
        for (const ExprPtr& arg : e->args) check_expr(arg.get(), t_int());
        e->resolved = "Bytes";
        return pool_.named(Type::K::class_, "Bytes");
    }
    if (name == "AtomicInt") {
        if (!e->type_args.empty())
            error_at(e->line, e->col, "AtomicInt takes no type arguments");
        require_count(1, "new AtomicInt");
        for (const ExprPtr& arg : e->args) check_expr(arg.get(), t_int());
        e->resolved = "AtomicInt";
        return pool_.named(Type::K::class_, "AtomicInt");
    }
    if (name == "Arena" || name == "Channel") {
        std::vector<TypeId> args = explicit_args();
        if (args.empty() && expected && expected->k == Type::K::class_ &&
            expected->name == name && expected->args.size() == 1)
            args = expected->args;
        if (args.size() != 1) {
            error_at(e->line, e->col, "new " + name +
                                          " needs one type argument or a declared result type");
        }
        require_count(1, "new " + name);
        for (const ExprPtr& arg : e->args) check_expr(arg.get(), t_int());
        e->resolved = name;
        return pool_.named(Type::K::class_, name,
                           args.size() == 1 ? args : std::vector<TypeId>{t_poison()});
    }
    if (name == "Box" || name == "Shared" || name == "Mutex") {
        if (!require_count(1, "new " + name)) return t_poison();
        std::vector<TypeId> args = explicit_args();
        TypeId inner = args.size() == 1 ? args[0] : nullptr;
        if (!inner && expected && expected->k == Type::K::class_ &&
            expected->name == name && expected->args.size() == 1)
            inner = expected->args[0];
        TypeId got = check_expr(e->args[0].get(), inner);
        if (!inner) inner = got;
        if (args.size() > 1)
            error_at(e->line, e->col, name + " takes one type argument");
        require_move_source(e->args[0].get(), got, "new " + name);
        if (!assignable(got, inner))
            error_at(e->args[0]->line, e->args[0]->col,
                     "new " + name + " needs " + type_name(inner) +
                         ", got " + type_name(got));
        e->resolved = name;
        return pool_.named(Type::K::class_, name, {inner});
    }

    std::string key = resolve_class_key(name, e->line, e->col);
    if (key.empty()) {
        error_at(e->line, e->col, "unknown class '" + name + "'");
        for (const ExprPtr& arg : e->args) check_expr(arg.get(), nullptr);
        return t_poison();
    }
    auto it = classes_.find(key);
    if (it == classes_.end() || !it->second.decl || it->second.decl->is_struct ||
        it->second.decl->is_union) {
        error_at(e->line, e->col, "new only builds classes; use a field literal for '" +
                                      name + "'");
        for (const ExprPtr& arg : e->args) check_expr(arg.get(), nullptr);
        return t_poison();
    }
    e->resolved = key;
    return check_ctor_call(e, key, name, expected);
}

// super.init(...): the child's own fields are already proven assigned by
// check_init_body's walk; here the call form, context, and args are checked,
// and the target class key lands in Expr::resolved for interp and codegen
TypeId Checker::check_super_init(const Expr* e) {
    if (!in_init_body_ || !cur_class_) {
        error_at(e->line, e->col, "super.init can only be called from init");
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
        return t_poison();
    }
    const ClassInfo* p = parent_class_of(*cur_class_);
    const ClassInfo* owner = nullptr;
    const FnDecl* ini = p ? chain_init(*p, &owner) : nullptr;
    if (!ini) {
        error_at(e->line, e->col,
                 "no parent constructor to call — " +
                     std::string(p ? "no class above declares an init"
                                   : "'" + cur_class_->decl->name + "' has no parent"));
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
        return t_poison();
    }
    check_pub(owner->decl->qualname, ini->is_pub, e->line, e->col, "init of",
              owner->decl->name);

    const std::vector<TypeId>& params = owner->methods.at("init")->fn_params;
    if (e->args.size() != params.size()) {
        error_at(e->line, e->col, "super.init takes " + std::to_string(params.size()) +
                                      " argument(s), got " + std::to_string(e->args.size()));
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
    } else {
        std::set<std::string> inout_names;
        for (size_t i = 0; i < e->args.size(); i++) {
            bool is_inout = i < ini->params.size() &&
                            ini->params[i].passing == Param::Passing::inout;
            bool saved_inout = allow_inout_expr_;
            allow_inout_expr_ = is_inout;
            TypeId got = check_expr(e->args[i].get(), params[i]);
            allow_inout_expr_ = saved_inout;
            if (i < ini->params.size() &&
                ini->params[i].passing == Param::Passing::move) {
                require_move_source(e->args[i].get(), params[i],
                                    "super.init move argument " + std::to_string(i + 1));
            }
            if (is_inout) {
                const Expr* arg = e->args[i].get();
                if (arg->kind != Expr::Kind::unary || arg->op != TokenKind::kw_inout ||
                    !arg->rhs || arg->rhs->kind != Expr::Kind::ident) {
                    error_at(arg->line, arg->col,
                             "super.init inout argument " + std::to_string(i + 1) +
                                 " must be 'inout var_name'");
                } else if (!inout_names.insert(std::string(arg->rhs->text)).second) {
                    error_at(arg->line, arg->col, "overlapping inout arguments");
                }
            }
            if (!assignable(got, params[i])) {
                error_at(e->args[i]->line, e->args[i]->col,
                         "super.init argument " + std::to_string(i + 1) + " is " +
                             type_name(params[i]) + ", got " + type_name(got));
            }
        }
    }
    e->callee->resolved = owner->decl->qualname;
    return t_unit();
}

TypeId Checker::check_ctor_call(const Expr* e, const std::string& key,
                                const std::string& shown, TypeId expected) {
    ClassInfo& c = classes_.at(key);
    if (c.decl->is_interface) {
        error_at(e->line, e->col, "'" + shown + "' is an interface — it can't be built");
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
        return t_poison();
    }
    // the constructor may be inherited: nearest class up the chain with an
    // init builds this class (legal because a class between them may not add
    // required fields — check_hierarchy enforced that)
    const ClassInfo* owner = nullptr;
    const FnDecl* ini = chain_init(c, &owner);

    // Type arguments may be explicit after the class name or inferred from the
    // declared result type.
    std::vector<TypeId> targs;
    for (const TypePtr& arg : e->type_args) targs.push_back(resolve_type(arg.get()));
    if (!c.generic_params.empty()) {
        if (targs.empty() && expected && expected->k == Type::K::class_ && expected->name == key &&
            expected->args.size() == c.generic_params.size()) {
            targs = expected->args;
        } else if (targs.empty()) {
            error_at(e->line, e->col, "can't tell the type arguments of '" + shown +
                                          "' here — write them or declare the result type");
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
            return t_poison();
        }
    } else if (!targs.empty()) {
        error_at(e->line, e->col, "'" + shown + "' takes no type arguments");
        targs.clear();
    }
    if (targs.size() != c.generic_params.size()) {
        error_at(e->line, e->col, "'" + shown + "' takes " +
                                      std::to_string(c.generic_params.size()) +
                                      " type argument(s), got " +
                                      std::to_string(targs.size()));
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
        return t_poison();
    }
    std::map<std::string, TypeId> env;
    for (size_t i = 0; i < targs.size(); i++) env[c.generic_params[i]] = targs[i];
    check_generic_bounds(c.decl->generics, env, e->line, e->col,
                         "new " + shown);

    // No declared init means the compiler-provided zero-argument initializer.
    // check_hierarchy has already proved every field has a default.
    if (!ini) {
        if (!e->args.empty()) {
            error_at(e->line, e->col, "implicit init of '" + shown +
                                          "' takes no arguments");
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
        }
        e->resolved = key;
        return pool_.named(Type::K::class_, key, std::move(targs));
    }
    check_pub(owner->decl->qualname, ini->is_pub, e->line, e->col, "init of", shown);

    // owner's signature: an inherited constructor can't mention this class's
    // type params (bases and interfaces take no type arguments), so subst only matters when
    // owner == the class itself
    TypeId ft = owner->methods.at("init");
    std::vector<TypeId> params;
    for (TypeId p : ft->fn_params) params.push_back(subst(p, env));

    if (e->args.size() != params.size()) {
        error_at(e->line, e->col, "'" + shown + "' init takes " +
                                      std::to_string(params.size()) + " argument(s), got " +
                                      std::to_string(e->args.size()));
        for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
    } else {
        std::set<std::string> inout_names;
        for (size_t i = 0; i < e->args.size(); i++) {
            bool is_inout = i < ini->params.size() &&
                            ini->params[i].passing == Param::Passing::inout;
            bool saved_inout = allow_inout_expr_;
            allow_inout_expr_ = is_inout;
            TypeId got = check_expr(e->args[i].get(), params[i]);
            allow_inout_expr_ = saved_inout;
            if (i < ini->params.size() &&
                ini->params[i].passing == Param::Passing::move) {
                require_move_source(e->args[i].get(), params[i],
                                    "'" + shown + "' init move argument " +
                                        std::to_string(i + 1));
            }
            if (is_inout) {
                const Expr* arg = e->args[i].get();
                if (arg->kind != Expr::Kind::unary || arg->op != TokenKind::kw_inout ||
                    !arg->rhs || arg->rhs->kind != Expr::Kind::ident) {
                    error_at(arg->line, arg->col,
                             "'" + shown + "' init inout argument " +
                                 std::to_string(i + 1) + " must be 'inout var_name'");
                } else if (!inout_names.insert(std::string(arg->rhs->text)).second) {
                    error_at(arg->line, arg->col, "overlapping inout arguments");
                }
            }
            if (!assignable(got, params[i])) {
                error_at(e->args[i]->line, e->args[i]->col,
                         "'" + shown + "' init argument " + std::to_string(i + 1) + " is " +
                             type_name(params[i]) + ", got " + type_name(got));
            }
        }
    }
    e->resolved = key; // interp and codegen construct straight from this
    return pool_.named(Type::K::class_, key, std::move(targs));
}

// The straight-line-prefix proof: walking init's top-level statements in
// order, a statement either assigns a field (its right side may only touch
// self through reads of fields already assigned) or doesn't touch self at
// all, and return is forbidden — until every field is assigned. This is what
// makes a half-built object unobservable without definite-assignment
// dataflow: conservative, one shape, both backends trust it.

bool Checker::expr_touches_self(const Expr* e, const std::set<std::string>& ok_fields) {
    if (!e) return false;
    if (e->kind == Expr::Kind::self_ref) return true;
    if (e->kind == Expr::Kind::string_lit &&
        e->text.find('{') != std::string_view::npos) {
        // interpolation segments are re-parsed after checking, so the walk
        // can't see inside them — assume the worst until every field is in
        return true;
    }
    if (e->kind == Expr::Kind::field && e->object &&
        e->object->kind == Expr::Kind::self_ref) {
        return !ok_fields.count(e->name); // reading an assigned field is fine
    }
    if (expr_touches_self(e->lhs.get(), ok_fields) ||
        expr_touches_self(e->rhs.get(), ok_fields) ||
        expr_touches_self(e->callee.get(), ok_fields) ||
        expr_touches_self(e->object.get(), ok_fields) ||
        expr_touches_self(e->index_expr.get(), ok_fields) ||
        expr_touches_self(e->cond.get(), ok_fields) ||
        expr_touches_self(e->then_e.get(), ok_fields) ||
        expr_touches_self(e->else_e.get(), ok_fields) ||
        expr_touches_self(e->subject.get(), ok_fields)) {
        return true;
    }
    for (const ExprPtr& a : e->args) {
        if (expr_touches_self(a.get(), ok_fields)) return true;
    }
    for (const InitEntry& en : e->entries) {
        if (expr_touches_self(en.key.get(), ok_fields) ||
            expr_touches_self(en.value.get(), ok_fields)) {
            return true;
        }
    }
    for (const MatchArm& m : e->arms) {
        if (expr_touches_self(m.value.get(), ok_fields) ||
            stmts_touch_self(m.body, ok_fields)) {
            return true;
        }
    }
    return stmts_touch_self(e->body, ok_fields); // closure body captures count
}

bool Checker::stmt_touches_self(const Stmt* s, const std::set<std::string>& ok_fields) {
    if (!s) return false;
    if (expr_touches_self(s->init.get(), ok_fields) ||
        expr_touches_self(s->target.get(), ok_fields) ||
        expr_touches_self(s->value.get(), ok_fields) ||
        expr_touches_self(s->expr.get(), ok_fields) ||
        expr_touches_self(s->cond.get(), ok_fields) ||
        expr_touches_self(s->iterable.get(), ok_fields)) {
        return true;
    }
    return stmts_touch_self(s->body, ok_fields) || stmts_touch_self(s->else_body, ok_fields);
}

bool Checker::stmts_touch_self(const std::vector<StmtPtr>& body,
                               const std::set<std::string>& ok_fields) {
    for (const StmtPtr& s : body) {
        if (stmt_touches_self(s.get(), ok_fields)) return true;
    }
    return false;
}

// is this expression the super.init(...) call form?
static bool is_super_init(const Expr* e) {
    return e && e->kind == Expr::Kind::call && e->callee &&
           e->callee->kind == Expr::Kind::field && e->callee->name == "init" &&
           e->callee->object && e->callee->object->kind == Expr::Kind::ident &&
           e->callee->object->text == "super";
}

// find super.init call sites other than the sanctioned one, anywhere in the
// body — a branch, a loop, a closure, after the first. Exactly-once and
// top-level are structural rules, so the walk is structural too.
static void scan_stray_super(const Expr* e, const Expr* ok,
                             std::vector<const Expr*>& out);
static void scan_stray_super(const std::vector<StmtPtr>& body, const Expr* ok,
                             std::vector<const Expr*>& out) {
    for (const StmtPtr& s : body) {
        scan_stray_super(s->init.get(), ok, out);
        scan_stray_super(s->target.get(), ok, out);
        scan_stray_super(s->value.get(), ok, out);
        scan_stray_super(s->expr.get(), ok, out);
        scan_stray_super(s->cond.get(), ok, out);
        scan_stray_super(s->iterable.get(), ok, out);
        scan_stray_super(s->body, ok, out);
        scan_stray_super(s->else_body, ok, out);
    }
}
static void scan_stray_super(const Expr* e, const Expr* ok,
                             std::vector<const Expr*>& out) {
    if (!e) return;
    if (is_super_init(e) && e != ok) out.push_back(e);
    scan_stray_super(e->lhs.get(), ok, out);
    scan_stray_super(e->rhs.get(), ok, out);
    scan_stray_super(e->callee.get(), ok, out);
    scan_stray_super(e->object.get(), ok, out);
    scan_stray_super(e->index_expr.get(), ok, out);
    scan_stray_super(e->cond.get(), ok, out);
    scan_stray_super(e->then_e.get(), ok, out);
    scan_stray_super(e->else_e.get(), ok, out);
    scan_stray_super(e->subject.get(), ok, out);
    for (const ExprPtr& a : e->args) scan_stray_super(a.get(), ok, out);
    for (const InitEntry& en : e->entries) {
        scan_stray_super(en.key.get(), ok, out);
        scan_stray_super(en.value.get(), ok, out);
    }
    for (const MatchArm& m : e->arms) {
        scan_stray_super(m.value.get(), ok, out);
        scan_stray_super(m.body, ok, out);
    }
    scan_stray_super(e->body, ok, out);
}

void Checker::check_init_body(const FnDecl& f, ClassInfo& cls) {
    std::set<std::string> assigned, own_all, all;
    for (const auto& [fname, fdecl] : cls.field_decls) {
        own_all.insert(fname);
        all.insert(fname);
        if (fdecl->def) assigned.insert(fname); // defaults are already there
    }

    // inherited fields: with an ancestor init they arrive through
    // super.init; without one, this init owns them like the raw form would
    const ClassInfo* parent = parent_class_of(cls);
    const FnDecl* anc_init = parent ? chain_init(*parent, nullptr) : nullptr;
    std::vector<std::string> inherited;
    for (const ClassInfo* k = parent; k; k = parent_class_of(*k)) {
        for (const auto& [fname, fdecl] : k->field_decls) {
            if (all.count(fname)) continue;
            all.insert(fname);
            inherited.push_back(fname);
            if (anc_init) continue; // super.init assigns these, defaults included
            size_t dot = k->decl->qualname.find('.');
            std::string owner_pkg =
                dot == std::string::npos ? "" : k->decl->qualname.substr(0, dot);
            if (fdecl->def) {
                assigned.insert(fname);
            } else if (owner_pkg != cur_pkg_ && !fdecl->is_pub) {
                error_at(f.line, f.col, "can't declare init here — inherited field '" +
                                            fname + "' isn't pub and has no default");
                assigned.insert(fname); // stop cascading
            }
        }
    }

    const Expr* sanctioned = nullptr; // the one top-level super.init
    for (const StmtPtr& sp : f.body) {
        // completion needs every field AND the parent's constructor run —
        // its body may do post-prefix work the parent's invariants rely on
        if (assigned.size() == all.size() && (!anc_init || sanctioned)) break;
        const Stmt* s = sp.get();

        if (anc_init && s->kind == Stmt::Kind::expr && is_super_init(s->expr.get())) {
            if (sanctioned) {
                error_at(s->line, s->col, "super.init runs once");
                continue;
            }
            for (const std::string& fname : own_all) {
                if (!assigned.count(fname)) {
                    error_at(s->line, s->col, "assign this class's own fields before "
                                              "super.init ('" +
                                                  fname + "' isn't assigned yet)");
                    break;
                }
            }
            for (const ExprPtr& a : s->expr->args) {
                if (expr_touches_self(a.get(), assigned)) {
                    error_at(a->line, a->col, "super.init arguments can only read "
                                              "fields already assigned");
                }
            }
            sanctioned = s->expr.get();
            for (const std::string& fname : inherited) assigned.insert(fname);
            continue;
        }

        if (s->kind == Stmt::Kind::assign && s->target &&
            s->target->kind == Expr::Kind::field && s->target->object &&
            s->target->object->kind == Expr::Kind::self_ref) {
            const std::string& fname = s->target->name;
            if (anc_init && !own_all.count(fname) && all.count(fname)) {
                error_at(s->line, s->col, "'" + fname + "' belongs to the parent — "
                                          "super.init assigns it");
                continue;
            }
            if (s->op != TokenKind::assign && !assigned.count(fname)) {
                error_at(s->line, s->col,
                         "init reads '" + fname + "' before assigning it");
            }
            if (expr_touches_self(s->value.get(), assigned)) {
                error_at(s->line, s->col, "init can only read fields it has already "
                                          "assigned — assign every field first");
            }
            if (all.count(fname)) assigned.insert(fname);
            continue;
        }
        if (s->kind == Stmt::Kind::ret) {
            error_at(s->line, s->col,
                     anc_init && !sanctioned
                         ? "init can't return before super.init"
                         : "init can't return before every field is assigned");
            continue;
        }
        if (stmt_touches_self(s, assigned)) {
            std::string missing;
            for (const std::string& fname : all) {
                if (!assigned.count(fname)) { missing = fname; break; }
            }
            error_at(s->line, s->col, "init must assign every field before using self ('" +
                                          missing + "' isn't assigned yet)");
        }
    }

    if (anc_init) {
        if (!sanctioned) {
            error_at(f.line, f.col, "init must call super.init(...) — the parent's "
                                    "constructor sets up its part");
        }
        std::vector<const Expr*> stray;
        scan_stray_super(f.body, sanctioned, stray);
        for (const Expr* s : stray) {
            error_at(s->line, s->col, "super.init runs once, as a top-level statement "
                                      "of init");
        }
    }

    if (assigned.size() != all.size()) {
        for (const std::string& fname : all) {
            if (!assigned.count(fname)) {
                error_at(f.line, f.col, "init never assigns field '" + fname + "'");
            }
        }
    }
}

// initializers ----------------------------------------------------------------

TypeId Checker::check_init(const Expr* e, TypeId expected) {
    std::string cname = e->name;
    std::vector<TypeId> targs;
    for (const TypePtr& t : e->type_args) targs.push_back(resolve_type(t.get()));

    // A struct literal can take its shape from the expected type.
    if (cname.empty()) {
        if (!expected) {
            error_at(e->line, e->col,
                     "can't tell what this { } builds — the spot needs a declared type");
            for (const InitEntry& en : e->entries) check_expr(en.value.get(), nullptr);
            return t_poison();
        }
        if (expected->k == Type::K::class_ &&
            (expected->name == "Map" || expected->name == "OrderedMap") &&
            expected->args.size() == 2) {
            TypeId K = expected->args[0], V = expected->args[1];
            for (const InitEntry& en : e->entries) {
                if (!en.name.empty()) {
                    error_at(e->line, e->col,
                             "map keys are values — write them in quotes or as expressions");
                    check_expr(en.value.get(), V);
                    continue;
                }
                TypeId kt = check_expr(en.key.get(), K);
                require_move_source(en.key.get(), kt, "map literal key");
                if (!assignable(kt, K)) {
                    error_at(en.key->line, en.key->col,
                             "map key is " + type_name(K) + ", got " + type_name(kt));
                }
                TypeId vt = check_expr(en.value.get(), V);
                require_move_source(en.value.get(), vt, "map literal value");
                if (!assignable(vt, V)) {
                    error_at(en.value->line, en.value->col,
                             "map value is " + type_name(V) + ", got " + type_name(vt));
                }
            }
            return expected;
        }
        if ((expected->k == Type::K::class_ || expected->k == Type::K::struct_) &&
            classes_.count(expected->name)) {
            cname = expected->name; // already a qualified key from the type pool
            targs = expected->args;
        } else {
            error_at(e->line, e->col, "can't build a " + type_name(expected) + " with { }");
            for (const InitEntry& en : e->entries) check_expr(en.value.get(), nullptr);
            return t_poison();
        }
    } else {
        // source name (`User` or `util.User`) -> qualified key
        std::string key = resolve_class_key(cname, e->line, e->col);
        if (key.empty()) {
            error_at(e->line, e->col, "unknown type '" + cname + "' in initializer");
            for (const InitEntry& en : e->entries) check_expr(en.value.get(), nullptr);
            return t_poison();
        }
        cname = key;
    }
    e->resolved = cname; // interp and codegen build straight from this

    auto cit = classes_.find(cname);
    if (cit == classes_.end()) {
        error_at(e->line, e->col, "unknown type '" + cname + "' in initializer");
        for (const InitEntry& en : e->entries) check_expr(en.value.get(), nullptr);
        return t_poison();
    }
    ClassInfo& c = cit->second;
    if (c.decl->is_interface) {
        error_at(e->line, e->col, "'" + cname + "' is an interface — it can't be built directly");
        return t_poison();
    }
    if (!c.decl->is_struct && !c.decl->is_union) {
        error_at(e->line, e->col,
                 "classes are built with 'new " + c.decl->name + "(...)'; "
                 "field literals are only for structs");
        for (const InitEntry& en : e->entries) check_expr(en.value.get(), nullptr);
        return t_poison();
    }
    if (c.decl->is_union) {
        if (unsafe_depth_ == 0)
            error_at(e->line, e->col, "union initialization requires unsafe { }");
        if (e->entries.size() != 1)
            error_at(e->line, e->col,
                     "union initializer sets exactly one field, got " +
                         std::to_string(e->entries.size()));
    }
    if (targs.empty() && !c.generic_params.empty() && expected &&
        (expected->k == Type::K::class_ || expected->k == Type::K::struct_) &&
        expected->name == cname) {
        targs = expected->args;
    }
    if (targs.size() != c.generic_params.size()) {
        error_at(e->line, e->col, "'" + cname + "' takes " +
                                      std::to_string(c.generic_params.size()) +
                                      " type argument(s)");
        return t_poison();
    }
    std::map<std::string, TypeId> env;
    for (size_t i = 0; i < targs.size(); i++) env[c.generic_params[i]] = targs[i];

    // all settable fields: own (with generic subst) plus everything inherited
    std::map<std::string, TypeId> all_fields;
    std::map<std::string, const FieldDecl*> all_field_decls;
    std::map<std::string, std::string> all_field_owner; // declaring class's key
    {
        std::set<std::string> seen;
        std::vector<const ClassInfo*> work = {&c};
        bool own = true;
        while (!work.empty()) {
            const ClassInfo* cc = work.back();
            work.pop_back();
            if (!seen.insert(cc->decl->qualname).second) continue;
            for (const auto& [fname, ftype] : cc->fields) {
                if (!all_fields.count(fname)) {
                    all_fields[fname] = own ? subst(ftype, env) : ftype;
                    all_field_decls[fname] = cc->field_decls.at(fname);
                    all_field_owner[fname] = cc->decl->qualname;
                }
            }
            for (const std::string& s : cc->supers) {
                auto sit = classes_.find(s);
                if (sit != classes_.end()) work.push_back(&sit->second);
            }
            own = false;
        }
    }
    // a field is settable here if its declaring class's package wrote it pub
    // (initializers must not reach private fields another package hid)
    auto field_visible = [&](const std::string& fname) {
        const std::string& owner_key = all_field_owner[fname];
        size_t dot = owner_key.find('.');
        std::string owner = dot == std::string::npos ? "" : owner_key.substr(0, dot);
        return owner == cur_pkg_ || all_field_decls[fname]->is_pub;
    };

    std::set<std::string> given;
    for (const InitEntry& en : e->entries) {
        if (en.name.empty()) {
            error_at(e->line, e->col, "class initializers use field: value");
            check_expr(en.value.get(), nullptr);
            continue;
        }
        auto fit = all_fields.find(en.name);
        if (fit == all_fields.end()) {
            error_at(e->line, e->col, cname + " has no field '" + en.name + "'");
            check_expr(en.value.get(), nullptr);
            continue;
        }
        if (!field_visible(en.name)) {
            error_at(e->line, e->col, "field '" + cname + "." + en.name +
                                          "' isn't pub in package '" +
                                          all_field_owner[en.name].substr(
                                              0, all_field_owner[en.name].find('.')) +
                                          "'");
        }
        given.insert(en.name);
        TypeId want = fit->second;
        TypeId got = check_expr(en.value.get(), want);
        require_move_source(en.value.get(), got, "field '" + en.name + "'");
        if (!assignable(got, want)) {
            error_at(en.value->line, en.value->col, "field '" + en.name + "' is " +
                                                        type_name(want) + ", got " +
                                                        type_name(got));
        }
    }
    for (const auto& [fname, fdecl] : all_field_decls) {
        if (c.decl->is_union) break;
        if (!fdecl->def && !given.count(fname)) {
            if (!field_visible(fname)) {
                error_at(e->line, e->col, "can't build '" + cname + "' here — field '" +
                                              fname + "' isn't pub and has no default");
            } else {
                error_at(e->line, e->col, "missing field '" + fname + "' (it has no default)");
            }
        }
    }
    return pool_.named((c.decl->is_struct || c.decl->is_union)
                           ? Type::K::struct_
                           : Type::K::class_,
                       cname, std::move(targs));
}

// match -----------------------------------------------------------------------

TypeId Checker::check_match(const Expr* e, TypeId expected, bool as_stmt) {
    TypeId subj = check_expr(e->subject.get(), nullptr);

    const EnumInfo* einfo = nullptr;
    std::map<std::string, TypeId> env;
    if (subj->k == Type::K::enum_) {
        auto it = enums_.find(subj->name);
        if (it != enums_.end()) {
            einfo = &it->second;
            for (size_t i = 0; i < einfo->generic_params.size() && i < subj->args.size(); i++) {
                env[einfo->generic_params[i]] = subj->args[i];
            }
        }
    }

    std::set<std::string> covered;
    bool has_wild = false;
    bool saw_true = false, saw_false = false;
    TypeId result = expected;
    auto move_base = scopes_;
    std::vector<std::vector<std::map<std::string, Local>>> continuing_states;

    // pattern checking, recursing through | alternatives
    std::function<void(const Pattern*)> check_pat = [&](const Pattern* p) {
        switch (p->kind) {
            case Pattern::Kind::wildcard:
                has_wild = true;
                break;
            case Pattern::Kind::alt:
                for (const PatPtr& a : p->alts) check_pat(a.get());
                break;
            case Pattern::Kind::literal:
            case Pattern::Kind::range: {
                TypeId lt = check_expr(p->lit.get(), subj);
                if (p->kind == Pattern::Kind::range) check_expr(p->lit2.get(), subj);
                if (subj->k != Type::K::poison && lt->k != Type::K::poison && lt != subj) {
                    error_at(p->line, p->col, "pattern is " + type_name(lt) +
                                                  " but the match subject is " + type_name(subj));
                }
                if (lt->k == Type::K::bool_ && p->lit->kind == Expr::Kind::bool_lit) {
                    (p->lit->bool_val ? saw_true : saw_false) = true;
                }
                break;
            }
            case Pattern::Kind::name: {
                if (!einfo) {
                    error_at(p->line, p->col, "'" + p->name +
                                                  "' pattern needs an enum subject, this is " +
                                                  type_name(subj));
                    break;
                }
                auto vit = einfo->variants.find(p->name);
                if (vit == einfo->variants.end()) {
                    error_at(p->line, p->col, type_name(subj) + " has no variant '" + p->name + "'");
                    break;
                }
                covered.insert(p->name);
                const std::vector<TypeId>& payload = vit->second;
                if (p->has_payload || !p->bindings.empty()) {
                    if (p->bindings.size() != payload.size()) {
                        error_at(p->line, p->col, "variant '" + p->name + "' carries " +
                                                      std::to_string(payload.size()) +
                                                      " value(s), pattern binds " +
                                                      std::to_string(p->bindings.size()));
                    }
                    for (size_t i = 0; i < p->bindings.size() && i < payload.size(); i++) {
                        TypeId bt = subst(payload[i], env);
                        if (p->bindings[i].type) {
                            TypeId annotated = resolve_type(p->bindings[i].type.get());
                            if (annotated != bt && annotated->k != Type::K::poison) {
                                error_at(p->bindings[i].line, p->bindings[i].col,
                                         "binding '" + p->bindings[i].name + "' is " +
                                             type_name(bt) + ", not " + type_name(annotated));
                            }
                        }
                        declare(p->bindings[i].name, bt, false,
                                p->bindings[i].line, p->bindings[i].col, true);
                    }
                } else if (!payload.empty()) {
                    error_at(p->line, p->col, "variant '" + p->name + "' carries data — bind it: " +
                                                  p->name + "(...)");
                }
                break;
            }
        }
    };

    for (const MatchArm& arm : e->arms) {
        scopes_ = move_base;
        push_scope();
        check_pat(arm.pat.get());
        if (arm.is_block) {
            if (!as_stmt) {
                error_at(arm.pat->line, arm.pat->col,
                         "a block arm doesn't produce a value — this match is used as one. "
                         "use `pattern => expression` arms here");
            }
            check_block(arm.body);
            pop_scope();
            if (!always_returns(arm.body)) continuing_states.push_back(scopes_);
            continue;
        }
        TypeId at = check_expr(arm.value.get(), as_stmt ? nullptr : result);
        pop_scope();
        continuing_states.push_back(scopes_);
        if (as_stmt) continue; // value is dropped, arms may differ
        if (!result) {
            result = at;
        } else if (assignable(at, result)) {
            // fine
        } else if (assignable(result, at)) {
            result = at;
        } else {
            error_at(arm.value->line, arm.value->col,
                     "match arms disagree: " + type_name(result) + " vs " + type_name(at));
        }
    }

    scopes_ = move_base;
    if (!continuing_states.empty()) {
        auto merged = continuing_states[0];
        for (size_t i = 1; i < continuing_states.size(); i++) {
            scopes_ = move_base;
            merge_move_states(merged, continuing_states[i]);
            merged = scopes_;
        }
        scopes_ = std::move(merged);
    }

    // exhaustiveness
    if (!has_wild && subj->k != Type::K::poison) {
        if (einfo) {
            std::string missing;
            for (const std::string& v : einfo->variant_order) {
                if (!covered.count(v)) missing += (missing.empty() ? "" : ", ") + v;
            }
            if (!missing.empty()) {
                error_at(e->line, e->col,
                         "match doesn't cover: " + missing + " — add them or a _ arm");
            }
        } else if (subj->k == Type::K::bool_) {
            if (!saw_true || !saw_false) {
                error_at(e->line, e->col, "match on bool needs true and false (or _)");
            }
        } else {
            error_at(e->line, e->col, "match on " + type_name(subj) + " needs a _ arm");
        }
    }

    return result ? result : t_unit();
}

// string interpolation ---------------------------------------------------------

void Checker::check_interpolation(const Expr* str) {
    std::string_view raw = str->text;
    if (raw.size() < 2) return;
    std::string_view body = raw.substr(1, raw.size() - 2);

    size_t i = 0;
    while (i < body.size()) {
        char c = body[i];
        if (c == '\\') { i += 2; continue; }
        if (c != '{') { i += 1; continue; }

        // find the matching close, skipping nested braces and inner strings
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
        if (depth != 0) return; // lexer already reported the malformed string
        std::string segment(body.substr(start, j - 1 - start));
        i = j;

        if (segment.empty()) {
            error_at(str->line, str->col, "empty {} in string");
            continue;
        }

        // "{expr:8.2}" — strip a format spec before parsing the expr
        FmtSpec spec;
        std::string ferr;
        std::string expr_text(split_fmt_spec(segment, spec, &ferr));
        if (!ferr.empty()) {
            error_at(str->line, str->col,
                     "in string piece {" + segment + "}: " + ferr);
            continue;
        }

        Lexer sub_lexer(expr_text);
        std::vector<Token> toks = sub_lexer.scan_all();
        Parser sub_parser(std::move(toks));
        ExprPtr seg = sub_parser.parse_standalone_expr();
        for (const LexError& le : sub_lexer.errors()) {
            error_at(str->line, str->col, "in string piece {" + segment + "}: " + le.msg);
        }
        for (const ParseError& pe : sub_parser.errors()) {
            error_at(str->line, str->col, "in string piece {" + segment + "}: " + pe.msg);
        }
        if (!sub_lexer.errors().empty() || !sub_parser.errors().empty()) continue;

        // errors inside the segment carry its private coordinates —
        // re-anchor them to the string literal and say where they came from
        size_t before = errors_.size();
        TypeId t = check_expr(seg.get(), nullptr);
        for (size_t k = before; k < errors_.size(); k++) {
            errors_[k].msg = "in string piece {" + segment + "}: " + errors_[k].msg;
            errors_[k].line = str->line;
            errors_[k].col = str->col;
        }
        if (!printable(t)) {
            error_at(str->line, str->col, "can't put a " + type_name(t) +
                                              " inside a string yet — give it a string form first");
        }
        if (spec.has && spec.places >= 0 &&
            !(t->k == Type::K::f64_ || t->k == Type::K::f32 ||
              t->k == Type::K::decimal_ || t->k == Type::K::poison)) {
            error_at(str->line, str->col, "in string piece {" + segment +
                                              "}: places (.N) need a float or decimal, this is " +
                                              type_name(t));
        }
        hir_.forget_expr_tree(seg.get());
    }
}

} // namespace beans
