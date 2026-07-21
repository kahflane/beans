#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "types.h"

namespace beans {

struct CheckError {
    std::string msg;
    uint32_t line;
    uint32_t col;
};

// Semantic info for a user class or interface.
struct ClassInfo {
    const ClassDecl* decl = nullptr;
    std::vector<std::string> generic_params;
    std::vector<std::string> supers;
    std::map<std::string, TypeId> fields;         // declared types (may contain type params)
    std::map<std::string, const FieldDecl*> field_decls;
    std::map<std::string, TypeId> methods;        // fn types
    std::map<std::string, const FnDecl*> method_decls;
};

struct EnumInfo {
    const EnumDecl* decl = nullptr; // null for builtin Option/Result
    std::vector<std::string> generic_params;
    std::vector<std::string> variant_order;
    std::map<std::string, std::vector<TypeId>> variants; // payload types
    std::map<std::string, TypeId> methods;
    std::map<std::string, const FnDecl*> method_decls;
};

class Checker {
public:
    explicit Checker(const Module& mod);

    void run();
    const std::vector<CheckError>& errors() const { return errors_; }

private:
    // ---- setup ----
    void register_builtins();
    void register_decls();     // pass 1: names + signatures
    void check_bodies();       // pass 2

    // ---- declarations ----
    void resolve_class_members(ClassInfo& c);
    void resolve_enum_members(EnumInfo& e);
    void check_hierarchy(ClassInfo& c);
    void check_fn_body(const FnDecl& f, ClassInfo* cls, EnumInfo* en);

    // ---- types ----
    TypeId resolve_type(const TypeRef* t);
    TypeId subst(TypeId t, const std::map<std::string, TypeId>& map);
    bool unify(TypeId param, TypeId arg, std::map<std::string, TypeId>& out);
    bool assignable(TypeId from, TypeId to);
    bool is_subclass_of(const std::string& cls, const std::string& super);
    bool printable(TypeId t);

    // ---- statements ----
    void check_block(const std::vector<StmtPtr>& body);
    void check_stmt(const Stmt* s);

    // ---- expressions ----
    TypeId check_expr(const Expr* e, TypeId expected);
    TypeId check_call(const Expr* e, TypeId expected);
    TypeId check_init(const Expr* e, TypeId expected);
    TypeId check_field(const Expr* e, bool for_call, TypeId* self_type_out);
    TypeId check_match(const Expr* e, TypeId expected);
    TypeId check_binary(const Expr* e);
    TypeId literal_or(const Expr* e, TypeId expected, TypeId dflt);
    bool is_adaptable_literal(const Expr* e);
    void check_interpolation(const Expr* str);

    // member lookup on a value of type `recv`
    struct Member {
        enum class Kind { none, field, method } kind = Kind::none;
        TypeId type = nullptr;   // field type or fn type
        bool is_static = false;
    };
    Member lookup_member(TypeId recv, const std::string& name);
    Member builtin_member(TypeId recv, const std::string& name);
    TypeId class_type_of(const ClassInfo& c); // self type incl. own params

    // ---- scopes ----
    struct Local { TypeId type; bool mut; };
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }
    void declare(const std::string& name, TypeId t, bool mut, uint32_t line, uint32_t col);
    const Local* find_local(const std::string& name) const;

    void error_at(uint32_t line, uint32_t col, std::string msg);

    // ---- state ----
    const Module& mod_;
    TypePool pool_;
    std::vector<CheckError> errors_;

    std::map<std::string, ClassInfo> classes_;
    std::map<std::string, EnumInfo> enums_;
    std::map<std::string, TypeId> top_fns_;
    std::map<std::string, const FnDecl*> top_fn_decls_;
    std::map<std::string, std::string> pkg_paths_; // bound name -> import path
    std::set<std::string> builtin_generic_classes_; // List, Map, Thread, Mutex, Channel

    std::vector<std::map<std::string, Local>> scopes_;

    // current context
    ClassInfo* cur_class_ = nullptr;
    EnumInfo* cur_enum_ = nullptr;
    TypeId cur_ret_ = nullptr;
    bool cur_has_self_ = false;
    std::set<std::string> cur_type_params_;

    // shorthands
    TypeId t_int() { return pool_.prim(Type::K::int_); }
    TypeId t_f64() { return pool_.prim(Type::K::f64_); }
    TypeId t_dec() { return pool_.prim(Type::K::decimal_); }
    TypeId t_bool() { return pool_.prim(Type::K::bool_); }
    TypeId t_str() { return pool_.prim(Type::K::string_); }
    TypeId t_unit() { return pool_.prim(Type::K::unit); }
    TypeId t_poison() { return pool_.prim(Type::K::poison); }
    TypeId t_option(TypeId inner) { return pool_.named(Type::K::enum_, "Option", {inner}); }
    TypeId t_result(TypeId ok, TypeId err) { return pool_.named(Type::K::enum_, "Result", {ok, err}); }
    TypeId t_error_class() { return pool_.named(Type::K::class_, "Error"); }
};

} // namespace beans
