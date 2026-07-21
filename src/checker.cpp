#include "checker.h"

#include <functional>
#include <utility>

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

Checker::Checker(const Program& prog) : prog_(prog) {}

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
            n == "Result" || n == "AtomicInt") {
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
    builtin_generic_classes_ = {"List", "Map", "Thread", "Mutex", "Channel"};

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
                    resolve_supers(it->second);
                    resolve_class_members(it->second);
                }
            }
            for (const EnumDecl& e : pf->mod.enums) {
                auto it = enums_.find(e.qualname);
                if (it != enums_.end() && it->second.decl == &e) {
                    resolve_enum_members(it->second);
                }
            }
            for (const FnDecl& f : pf->mod.fns) {
                if (top_fns_.count(f.qualname)) {
                    error_at(f.line, f.col, "function '" + f.name + "' defined twice");
                    continue;
                }
                cur_type_params_.clear();
                for (const GenericParam& g : f.generics) cur_type_params_.insert(g.name);
                std::vector<TypeId> params;
                for (const Param& p : f.params) params.push_back(resolve_type(p.type.get()));
                TypeId ret = f.ret ? resolve_type(f.ret.get()) : t_unit();
                top_fns_[f.qualname] = pool_.fn(std::move(params), ret);
                top_fn_decls_[f.qualname] = &f;
                cur_type_params_.clear();
            }
        }
    }
}

// class parents may be local names or `pkg.Name` — pin them to keys once,
// and write them into the decl for the interpreter and codegen
void Checker::resolve_supers(ClassInfo& c) {
    c.supers.clear();
    c.decl->supers_resolved.clear();
    for (const std::string& s : c.decl->supers) {
        std::string key = resolve_class_key(s, c.decl->line, c.decl->col);
        if (key.empty()) {
            error_at(c.decl->line, c.decl->col, "unknown parent '" + s + "'");
            continue;
        }
        c.supers.push_back(key);
        c.decl->supers_resolved.push_back(key);
    }
}

void Checker::resolve_class_members(ClassInfo& c) {
    cur_type_params_.clear();
    for (const std::string& g : c.generic_params) cur_type_params_.insert(g);

    for (const FieldDecl& f : c.decl->fields) {
        if (c.fields.count(f.name)) {
            error_at(f.line, f.col, "field '" + f.name + "' defined twice");
            continue;
        }
        c.fields[f.name] = resolve_type(f.type.get());
        c.field_decls[f.name] = &f;
    }
    for (const FnDecl& m : c.decl->methods) {
        if (c.methods.count(m.name) || c.fields.count(m.name)) {
            error_at(m.line, m.col, "member '" + m.name + "' defined twice");
            continue;
        }
        for (const GenericParam& g : m.generics) cur_type_params_.insert(g.name);
        std::vector<TypeId> params;
        for (const Param& p : m.params) params.push_back(resolve_type(p.type.get()));
        TypeId ret = m.ret ? resolve_type(m.ret.get()) : t_unit();
        c.methods[m.name] = pool_.fn(std::move(params), ret);
        c.method_decls[m.name] = &m;
        for (const GenericParam& g : m.generics) cur_type_params_.erase(g.name);
    }
    cur_type_params_.clear();
}

void Checker::resolve_enum_members(EnumInfo& e) {
    cur_type_params_.clear();
    for (const std::string& g : e.generic_params) cur_type_params_.insert(g);

    for (const EnumVariant& v : e.decl->variants) {
        if (e.variants.count(v.name)) {
            error_at(e.decl->line, e.decl->col, "variant '" + v.name + "' defined twice");
            continue;
        }
        std::vector<TypeId> payload;
        for (const Param& p : v.payload) payload.push_back(resolve_type(p.type.get()));
        e.variants[v.name] = std::move(payload);
        e.variant_order.push_back(v.name);
    }
    for (const FnDecl& m : e.decl->methods) {
        if (e.methods.count(m.name)) {
            error_at(m.line, m.col, "method '" + m.name + "' defined twice");
            continue;
        }
        std::vector<TypeId> params;
        for (const Param& p : m.params) params.push_back(resolve_type(p.type.get()));
        TypeId ret = m.ret ? resolve_type(m.ret.get()) : t_unit();
        e.methods[m.name] = pool_.fn(std::move(params), ret);
        e.method_decls[m.name] = &m;
    }
    cur_type_params_.clear();
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

void Checker::check_hierarchy(ClassInfo& c) {
    const ClassDecl* d = c.decl;
    int class_parents = 0;

    // collect inherited members
    std::map<std::string, TypeId> inh_concrete;   // concrete method or iface default
    std::map<std::string, bool> inh_concrete_self;
    std::map<std::string, TypeId> inh_sigs;       // iface signatures without bodies

    std::set<std::string> seen = {d->qualname}; // supers are qualified keys
    std::vector<std::string> work = c.supers;
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
                }
            } else {
                if (!inh_sigs.count(mname)) inh_sigs[mname] = mtype;
            }
        }
        for (const std::string& s : p.supers) work.push_back(s);
    }

    // override rules
    for (const auto& [mname, mdecl] : c.method_decls) {
        if (!mdecl->has_self) continue;
        TypeId own = c.methods.at(mname);
        bool has_concrete = inh_concrete.count(mname) > 0;
        bool has_sig = inh_sigs.count(mname) > 0;
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
            if (has_concrete) {
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
        size_t want = n == "Map" ? 2 : 1;
        if (!want_args(want)) return t_poison();
        return pool_.named(Type::K::class_, n, resolved_args());
    }

    std::string ckey = resolve_class_key(n, t->line, t->col);
    if (!ckey.empty()) {
        auto cit = classes_.find(ckey);
        if (!want_args(cit->second.generic_params.size())) return t_poison();
        t->resolved = ckey;
        return pool_.named(Type::K::class_, ckey, resolved_args());
    }
    std::string ekey = resolve_enum_key(n, t->line, t->col);
    if (!ekey.empty()) {
        auto eit = enums_.find(ekey);
        if (!want_args(eit->second.generic_params.size())) return t_poison();
        t->resolved = ekey;
        return pool_.named(Type::K::enum_, ekey, resolved_args());
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
        case Type::K::enum_: {
            if (t->args.empty()) return t;
            std::vector<TypeId> args;
            for (TypeId a : t->args) args.push_back(subst(a, map));
            return pool_.named(t->k, t->name, std::move(args));
        }
        case Type::K::range:
            return pool_.range_of(subst(t->args[0], map));
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
        case Type::K::enum_: {
            if (param->name != arg->name || param->args.size() != arg->args.size()) return false;
            for (size_t i = 0; i < param->args.size(); i++) {
                if (!unify(param->args[i], arg->args[i], out)) return false;
            }
            return true;
        }
        case Type::K::range:
            return unify(param->args[0], arg->args[0], out);
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
    if (!t) return false;
    if (t->k == Type::K::poison) return true;
    if (t->is_numeric() || t->k == Type::K::bool_ || t->k == Type::K::string_) return true;
    if (t->k == Type::K::enum_ && t->args.empty()) return true; // plain enums print their name
    return false;
}

// ---- scopes ----------------------------------------------------------------

void Checker::declare(const std::string& name, TypeId t, bool mut,
                      uint32_t line, uint32_t col) {
    if (!scopes_.empty() && scopes_.back().count(name)) {
        error_at(line, col, "'" + name + "' is already defined in this scope");
        return;
    }
    scopes_.back()[name] = {t, mut};
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
                cur_type_params_.clear();
                for (const std::string& g : c.generic_params) cur_type_params_.insert(g);
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
    return pool_.named(Type::K::class_, c.decl->qualname, std::move(args));
}

void Checker::check_fn_body(const FnDecl& f, ClassInfo* cls, EnumInfo* en) {
    if (!f.has_body) return;

    cur_class_ = cls;
    cur_enum_ = en;
    cur_has_self_ = f.has_self;
    cur_type_params_.clear();
    if (cls) for (const std::string& g : cls->generic_params) cur_type_params_.insert(g);
    if (en) for (const std::string& g : en->generic_params) cur_type_params_.insert(g);
    for (const GenericParam& g : f.generics) cur_type_params_.insert(g.name);

    cur_ret_ = f.ret ? resolve_type(f.ret.get()) : t_unit();

    scopes_.clear();
    push_scope();
    for (const Param& p : f.params) {
        declare(p.name, resolve_type(p.type.get()), false, p.line, p.col);
    }
    check_block(f.body);
    pop_scope();

    cur_class_ = nullptr;
    cur_enum_ = nullptr;
    cur_has_self_ = false;
    cur_ret_ = nullptr;
    cur_type_params_.clear();
}

void Checker::check_block(const std::vector<StmtPtr>& body) {
    push_scope();
    for (const StmtPtr& s : body) check_stmt(s.get());
    pop_scope();
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
            const Expr* t = s->target.get();
            if (t->kind == Expr::Kind::ident) {
                const Local* l = find_local(std::string(t->text));
                if (!l) {
                    error_at(t->line, t->col, "unknown name '" + std::string(t->text) + "'");
                } else {
                    if (!l->mut) {
                        error_at(t->line, t->col, "'" + std::string(t->text) +
                                                      "' is a let — it can't be reassigned. use var");
                    }
                    target_t = l->type;
                }
            } else if (t->kind == Expr::Kind::field || t->kind == Expr::Kind::index) {
                target_t = check_expr(t, nullptr);
            } else {
                error_at(t->line, t->col, "can't assign to this expression");
            }

            TypeId val = check_expr(s->value.get(), target_t);
            if (s->op == TokenKind::assign) {
                if (!assignable(val, target_t)) {
                    error_at(s->line, s->col, "can't assign " + type_name(val) + " to " +
                                                  type_name(target_t));
                }
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
            check_block(s->body);
            if (!s->else_body.empty()) check_block(s->else_body);
            break;
        }
        case Stmt::Kind::for_ever:
            check_block(s->body);
            break;
        case Stmt::Kind::for_while: {
            TypeId c = check_expr(s->cond.get(), t_bool());
            if (c != t_bool() && c->k != Type::K::poison) {
                error_at(s->cond->line, s->cond->col,
                         "loop condition must be bool, got " + type_name(c));
            }
            check_block(s->body);
            break;
        }
        case Stmt::Kind::for_in: {
            TypeId it = check_expr(s->iterable.get(), nullptr);
            TypeId elem = t_poison();
            if (it->k == Type::K::range) {
                elem = it->args[0];
            } else if (it->k == Type::K::class_ && it->name == "List" && it->args.size() == 1) {
                elem = it->args[0];
            } else if (it->k != Type::K::poison) {
                error_at(s->iterable->line, s->iterable->col,
                         "can't loop over " + type_name(it) + " — expected a List or a range");
            }
            TypeId declared = resolve_type(s->loop_type.get());
            if (elem->k != Type::K::poison && declared != elem &&
                declared->k != Type::K::poison) {
                error_at(s->line, s->col, "loop variable is " + type_name(declared) +
                                              " but elements are " + type_name(elem));
            }
            push_scope();
            declare(s->loop_var, declared, false, s->line, s->col);
            check_block(s->body);
            pop_scope();
            break;
        }
        case Stmt::Kind::defer_:
            check_expr(s->expr.get(), nullptr);
            break;
        case Stmt::Kind::unsafe_:
            check_block(s->body);
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
    if (expected) {
        if (e->kind == Expr::Kind::int_lit && expected->is_numeric()) return expected;
        if (e->kind == Expr::Kind::float_lit &&
            (expected->is_float() || expected->k == Type::K::decimal_))
            return expected;
    }
    return dflt;
}

TypeId Checker::check_expr(const Expr* e, TypeId expected) {
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
            if (const Local* l = find_local(name)) return l->type;
            std::string fkey = resolve_fn_key(name);
            if (!fkey.empty()) {
                e->resolved = fkey;
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
            if (e->op == TokenKind::minus) {
                TypeId t = check_expr(e->rhs.get(), expected && expected->is_numeric() ? expected : nullptr);
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
        case Expr::Kind::call:
            return check_call(e, expected);
        case Expr::Kind::field:
            return check_field(e, false, nullptr);
        case Expr::Kind::index: {
            TypeId obj = check_expr(e->object.get(), nullptr);
            if (obj->k == Type::K::class_ && obj->name == "List" && obj->args.size() == 1) {
                TypeId idx = check_expr(e->index_expr.get(), t_int());
                if (!idx->is_int() && idx->k != Type::K::poison) {
                    error_at(e->line, e->col, "list index must be an integer");
                }
                return obj->args[0];
            }
            if (obj->k == Type::K::class_ && obj->name == "Map" && obj->args.size() == 2) {
                TypeId key = check_expr(e->index_expr.get(), obj->args[0]);
                if (!assignable(key, obj->args[0])) {
                    error_at(e->line, e->col, "map key is " + type_name(obj->args[0]) +
                                                  ", got " + type_name(key));
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
            if (expected && expected->k == Type::K::class_ && expected->name == "List" &&
                expected->args.size() == 1) {
                elem_expected = expected->args[0];
            }
            TypeId elem = elem_expected;
            for (const ExprPtr& el : e->args) {
                TypeId t = check_expr(el.get(), elem);
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
                return t->args[0];
            }
            if (t->k == Type::K::enum_ && t->name == "Option" && t->args.size() == 1) {
                if (!cur_ret_ || cur_ret_->k != Type::K::enum_ || cur_ret_->name != "Option") {
                    error_at(e->line, e->col,
                             "? on an Option needs the function to return Option");
                }
                return t->args[0];
            }
            error_at(e->line, e->col, "? works on Result or Option, got " + type_name(t));
            return t_poison();
        }
        case Expr::Kind::closure: {
            std::vector<TypeId> params;
            for (const Param& p : e->params) params.push_back(resolve_type(p.type.get()));
            TypeId ret = e->type ? resolve_type(e->type.get()) : t_unit();

            TypeId saved_ret = cur_ret_;
            cur_ret_ = ret;
            push_scope();
            for (size_t i = 0; i < e->params.size(); i++) {
                declare(e->params[i].name, params[i], false,
                        e->params[i].line, e->params[i].col);
            }
            check_block(e->body);
            pop_scope();
            cur_ret_ = saved_ret;
            return pool_.fn(std::move(params), ret);
        }
        case Expr::Kind::if_expr: {
            TypeId c = check_expr(e->cond.get(), t_bool());
            if (c != t_bool() && c->k != Type::K::poison) {
                error_at(e->cond->line, e->cond->col,
                         "condition must be bool, got " + type_name(c));
            }
            TypeId a = check_expr(e->then_e.get(), expected);
            TypeId b = check_expr(e->else_e.get(), expected ? expected : a);
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

    switch (op) {
        case TokenKind::eq:
        case TokenKind::neq: {
            bool comparable =
                l == r && (l->is_numeric() || l->k == Type::K::bool_ ||
                           l->k == Type::K::string_ ||
                           (l->k == Type::K::enum_ && l->args.empty()));
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
            bool ok = l == r && (l->is_numeric() || l->k == Type::K::string_);
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

    if (recv->k == Type::K::class_) {
        auto it = classes_.find(recv->name);
        if (it != classes_.end()) {
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
                if (mit != c->methods.end()) {
                    // a pub interface's methods travel with it — the interface
                    // IS its method set, hiding them would make it useless
                    bool open = c->method_decls.at(name)->is_pub ||
                                (c->decl->is_interface && c->decl->is_pub);
                    check_pub(c->decl->qualname, open, line, col, "method",
                              recv->name + "." + name);
                    Member m;
                    m.kind = Member::Kind::method;
                    m.type = first ? subst(mit->second, env) : mit->second;
                    m.is_static = !c->method_decls.at(name)->has_self;
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
                m.is_static = !it->second.method_decls.at(name)->has_self;
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
    if (recv->k == Type::K::string_) {
        if (name == "len") return method({}, t_int());
        if (name == "to_int") return method({}, t_result(t_int(), t_error_class()));
        if (name == "last") return method({t_int()}, t_str());
        if (name == "contains") return method({t_str()}, t_bool());
        return none;
    }
    if (recv->k == Type::K::class_) {
        if (recv->name == "List" && recv->args.size() == 1) {
            TypeId T = recv->args[0];
            if (name == "push") return method({T}, t_unit());
            if (name == "pop") return method({}, t_option(T));
            if (name == "get") return method({t_int()}, t_option(T));
            if (name == "len") return method({}, t_int());
            if (name == "max") return method({}, t_option(T));
            if (name == "contains") return method({T}, t_bool());
            if (name == "join") return method({t_str()}, t_str());
            return none;
        }
        if (recv->name == "Map" && recv->args.size() == 2) {
            TypeId K = recv->args[0], V = recv->args[1];
            if (name == "get") return method({K}, t_option(V));
            if (name == "set") return method({K, V}, t_unit());
            if (name == "len") return method({}, t_int());
            if (name == "contains") return method({K}, t_bool());
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
            return none;
        }
        if (recv->name == "Result" && recv->args.size() == 2) {
            TypeId T = recv->args[0];
            if (name == "or") return method({T}, T);
            if (name == "expect") return method({t_str()}, T);
            if (name == "is_ok") return method({}, t_bool());
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
                                  const std::string& what) -> TypeId {
        if (e->args.size() != params.size()) {
            error_at(e->line, e->col, what + " takes " + std::to_string(params.size()) +
                                          " argument(s), got " + std::to_string(e->args.size()));
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
            return ret;
        }
        for (size_t i = 0; i < e->args.size(); i++) {
            TypeId got = check_expr(e->args[i].get(), params[i]);
            if (!assignable(got, params[i])) {
                error_at(e->args[i]->line, e->args[i]->col,
                         what + " argument " + std::to_string(i + 1) + " is " +
                             type_name(params[i]) + ", got " + type_name(got));
            }
        }
        return ret;
    };

    // generic top-level fn: infer type params from the arguments
    auto call_generic_fn = [&](const FnDecl* decl, TypeId fn_t,
                               const std::string& what) -> TypeId {
        if (decl->generics.empty()) {
            return check_args_against(fn_t->fn_params, fn_t->fn_ret, what);
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
            TypeId got = check_expr(e->args[i].get(), nullptr);
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
        for (size_t i = 0; i < e->args.size(); i++) {
            TypeId want = subst(fn_t->fn_params[i], env);
            if (!assignable(arg_types[i], want)) {
                error_at(e->args[i]->line, e->args[i]->col,
                         what + " argument " + std::to_string(i + 1) + " is " +
                             type_name(want) + ", got " + type_name(arg_types[i]));
            }
        }
        return subst(fn_t->fn_ret, env);
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
                if (ok_t && !assignable(got, ok_t)) {
                    error_at(e->line, e->col, "ok(...) here needs " + type_name(ok_t) +
                                                  ", got " + type_name(got));
                }
                return t_result(ok_t ? ok_t : got, err_t);
            }
            TypeId got = check_expr(e->args[0].get(), nullptr);
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

            // package call
            auto pit = pkg_paths_.find(n);
            if (pit != pkg_paths_.end()) {
                const std::string& path = pit->second;
                if (path == "std.io" && (mname == "println" || mname == "print")) {
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
                if (path == "std.thread" && mname == "spawn") {
                    callee->resolved = "std.thread.spawn";
                    if (e->args.size() != 1) {
                        error_at(e->line, e->col, "thread.spawn takes 1 closure");
                        return t_poison();
                    }
                    TypeId f = check_expr(e->args[0].get(), nullptr);
                    if (f->k != Type::K::fn || !f->fn_params.empty()) {
                        error_at(e->line, e->col,
                                 "thread.spawn needs a closure with no parameters");
                        return t_poison();
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
                    error_at(e->line, e->col,
                             "constructing generic enum '" + n + "' this way isn't supported yet");
                    return t_poison();
                }
                return check_args_against(vit->second,
                                          pool_.named(Type::K::enum_, n),
                                          n + "." + mname);
            }

            // builtin statics
            if (n == "Mutex" && mname == "new") {
                if (e->args.size() != 1) {
                    error_at(e->line, e->col, "Mutex.new takes the value to guard");
                    return t_poison();
                }
                TypeId inner = nullptr;
                if (expected && expected->k == Type::K::class_ && expected->name == "Mutex")
                    inner = expected->args[0];
                TypeId got = check_expr(e->args[0].get(), inner);
                return pool_.named(Type::K::class_, "Mutex", {inner ? inner : got});
            }
            if (n == "Channel" && mname == "new") {
                if (e->args.size() != 1) {
                    error_at(e->line, e->col, "Channel.new takes a buffer size");
                }
                for (const ExprPtr& a : e->args) check_expr(a.get(), t_int());
                if (expected && expected->k == Type::K::class_ && expected->name == "Channel") {
                    return expected;
                }
                error_at(e->line, e->col,
                         "can't tell the element type — declare it: let ch: Channel<T> = ...");
                return t_poison();
            }
            if (n == "AtomicInt" && mname == "new") {
                for (const ExprPtr& a : e->args) check_expr(a.get(), t_int());
                return pool_.named(Type::K::class_, "AtomicInt");
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
                    error_at(e->line, e->col, "'" + mname + "' is an instance method — call it on a " + n + " value");
                    return t_poison();
                }
                check_pub(n, md->is_pub, e->line, e->col, "static", n + "." + mname);
                if (!cit->second.generic_params.empty()) {
                    error_at(e->line, e->col,
                             "statics on generic classes aren't supported yet — use an initializer");
                    return t_poison();
                }
                return check_args_against(mit->second->fn_params, mit->second->fn_ret,
                                          n + "." + mname);
            }
        }

        // plain instance method call
        TypeId t = check_expr(obj, nullptr);
        if (t->k == Type::K::poison) {
            for (const ExprPtr& a : e->args) check_expr(a.get(), nullptr);
            return t_poison();
        }
        Member m = lookup_member(t, mname, e->line, e->col);
        if (m.kind == Member::Kind::method) {
            if (m.is_static) {
                error_at(e->line, e->col, "'" + mname + "' is a static — call it on the type name");
                return t_poison();
            }
            return check_args_against(m.type->fn_params, m.type->fn_ret,
                                      type_name(t) + "." + mname);
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

// initializers ----------------------------------------------------------------

TypeId Checker::check_init(const Expr* e, TypeId expected) {
    std::string cname = e->name;
    std::vector<TypeId> targs;
    for (const TypePtr& t : e->type_args) targs.push_back(resolve_type(t.get()));

    // short form: take the shape from the expected type
    if (cname.empty()) {
        if (!expected) {
            error_at(e->line, e->col,
                     "can't tell what this { } builds — the spot needs a declared type");
            for (const InitEntry& en : e->entries) check_expr(en.value.get(), nullptr);
            return t_poison();
        }
        if (expected->k == Type::K::class_ && expected->name == "Map" &&
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
                if (!assignable(kt, K)) {
                    error_at(en.key->line, en.key->col,
                             "map key is " + type_name(K) + ", got " + type_name(kt));
                }
                TypeId vt = check_expr(en.value.get(), V);
                if (!assignable(vt, V)) {
                    error_at(en.value->line, en.value->col,
                             "map value is " + type_name(V) + ", got " + type_name(vt));
                }
            }
            return expected;
        }
        if (expected->k == Type::K::class_ && classes_.count(expected->name)) {
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
    if (targs.empty() && !c.generic_params.empty() && expected &&
        expected->k == Type::K::class_ && expected->name == cname) {
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
        if (!assignable(got, want)) {
            error_at(en.value->line, en.value->col, "field '" + en.name + "' is " +
                                                        type_name(want) + ", got " +
                                                        type_name(got));
        }
    }
    for (const auto& [fname, fdecl] : all_field_decls) {
        if (!fdecl->def && !given.count(fname)) {
            if (!field_visible(fname)) {
                error_at(e->line, e->col, "can't build '" + cname + "' here — field '" +
                                              fname + "' isn't pub and has no default");
            } else {
                error_at(e->line, e->col, "missing field '" + fname + "' (it has no default)");
            }
        }
    }
    return pool_.named(Type::K::class_, cname, std::move(targs));
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
                                p->bindings[i].line, p->bindings[i].col);
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
            continue;
        }
        TypeId at = check_expr(arm.value.get(), as_stmt ? nullptr : result);
        pop_scope();
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

        Lexer sub_lexer(segment);
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
    }
}

} // namespace beans
