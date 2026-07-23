#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "builtins.h"
#include "hir.h"
#include "types.h"

namespace beans {

struct CheckError {
    std::string msg;
    uint32_t line;
    uint32_t col;
    std::string file; // which file of the program; empty in single-file runs
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

// Whole-program checker. Every declaration is registered under its
// package-qualified name ("util.User"); the root package's prefix is empty, so
// single-file programs behave exactly as before. Cross-package references are
// resolved here once and written into the AST (Expr::resolved,
// TypeRef::resolved, ClassDecl::supers_resolved) so the interpreter and
// codegen never look at imports.
class Checker {
public:
    explicit Checker(const Program& prog);

    void run();
    const std::vector<CheckError>& errors() const { return errors_; }
    const HirProgram& hir() const { return hir_; }

private:
    // ---- setup ----
    void register_builtins();
    void register_decls();     // pass 1: names + signatures
    void check_bodies();       // pass 2
    void enter_file(const Package& pkg, const PFile& file); // sets context

    // ---- package-aware name resolution ----
    // key of a plain name in the current package ("util.User" / "User")
    std::string qual(const std::string& name) const;
    // source name -> internal key; handles "Name" and "pkg.Name" via the
    // current file's imports. Reports pub violations. "" = not found.
    std::string resolve_class_key(const std::string& n, uint32_t line, uint32_t col);
    std::string resolve_enum_key(const std::string& n, uint32_t line, uint32_t col);
    std::string resolve_fn_key(const std::string& n);
    // does `e` name a type (`Status`, `util.User`)? returns its key and
    // annotates e->resolved; "" otherwise
    std::string as_type_name(const Expr* e);
    // import binding of the current file? returns its path ("" if not)
    std::string import_path_of(const std::string& binding) const;
    bool check_pub(const std::string& key, bool is_pub, uint32_t line, uint32_t col,
                   const char* what, const std::string& shown);

    // ---- declarations ----
    void resolve_class_members(ClassInfo& c);
    void resolve_enum_members(EnumInfo& e);
    void resolve_supers(ClassInfo& c);
    void check_inline_layout_cycles();
    void check_hierarchy(ClassInfo& c);
    void check_fn_body(const FnDecl& f, ClassInfo* cls, EnumInfo* en);

    // ---- types ----
    TypeId resolve_type(const TypeRef* t);
    TypeId subst(TypeId t, const std::map<std::string, TypeId>& map);
    bool unify(TypeId param, TypeId arg, std::map<std::string, TypeId>& out);
    bool assignable(TypeId from, TypeId to);
    bool is_subclass_of(const std::string& cls, const std::string& super);
    bool printable(TypeId t);
    bool printable_rec(TypeId t, std::set<TypeId>& seen);
    bool trait_satisfied(TypeId t, const std::string& trait);
    bool trait_satisfied_rec(TypeId t, const std::string& trait,
                             std::set<std::pair<TypeId, std::string>>& seen);
    static std::vector<std::string> split_traits(const std::string& bounds);
    static std::string canonical_trait(const std::string& trait);
    void set_type_params(const std::vector<GenericParam>& params);
    void validate_generic_params(const std::vector<GenericParam>& params,
                                 uint32_t line, uint32_t col);
    void check_generic_bounds(const std::vector<GenericParam>& params,
                              const std::map<std::string, TypeId>& env,
                              uint32_t line, uint32_t col,
                              const std::string& what);
    bool is_move_only(TypeId t) const;
    bool is_wide_inline_value(TypeId t) const;
    bool is_inline_storage_type(TypeId t, bool require_c_layout) const;
    bool is_raw_pointee(TypeId t) const;
    bool is_c_abi_type(TypeId t, bool allow_unit) const;
    bool is_c_abi_callback(TypeId t) const;
    void require_move_source(const Expr* e, TypeId t, const std::string& where);

    // ---- statements ----
    void check_block(const std::vector<StmtPtr>& body);
    void check_stmt(const Stmt* s);

    // ---- missing return ----
    bool always_returns(const std::vector<StmtPtr>& body);
    bool stmt_returns(const Stmt* s);
    bool has_break(const std::vector<StmtPtr>& body);

    // ---- init / deinit ----
    // the class's own init, or null
    static const FnDecl* class_init(const ClassInfo& c);
    // nearest class in the parent chain (this one included) that declares an
    // init — the constructor Child(args) runs, and the super.init target when
    // starting above the class itself
    const FnDecl* chain_init(const ClassInfo& c, const ClassInfo** owner);
    const ClassInfo* parent_class_of(const ClassInfo& c);
    TypeId check_ctor_call(const Expr* e, const std::string& key,
                           const std::string& shown, TypeId expected);
    TypeId check_super_init(const Expr* e);
    // proves init's straight-line prefix assigns every field before self is used
    void check_init_body(const FnDecl& f, ClassInfo& cls);
    bool expr_touches_self(const Expr* e, const std::set<std::string>& ok_fields);
    bool stmt_touches_self(const Stmt* s, const std::set<std::string>& ok_fields);
    bool stmts_touch_self(const std::vector<StmtPtr>& body,
                          const std::set<std::string>& ok_fields);

    // ---- expressions ----
    TypeId check_expr(const Expr* e, TypeId expected);
    TypeId check_expr_impl(const Expr* e, TypeId expected);
    TypeId check_call(const Expr* e, TypeId expected);
    TypeId check_init(const Expr* e, TypeId expected);
    TypeId check_field(const Expr* e, bool for_call, TypeId* self_type_out);
    TypeId check_match(const Expr* e, TypeId expected, bool as_stmt = false);
    TypeId check_binary(const Expr* e);
    TypeId literal_or(const Expr* e, TypeId expected, TypeId dflt);
    void check_integer_literal(const Expr* e, TypeId type);
    bool is_adaptable_literal(const Expr* e);
    void check_interpolation(const Expr* str);

    // member lookup on a value of type `recv`
    struct Member {
        enum class Kind { none, field, method } kind = Kind::none;
        TypeId type = nullptr;   // field type or fn type
        bool is_static = false;
        const FnDecl* decl = nullptr; // user method; carries ownership modes
    };
    Member lookup_member(TypeId recv, const std::string& name,
                         uint32_t line = 0, uint32_t col = 0);
    Member builtin_member(TypeId recv, const std::string& name);
    TypeId bt_type(BT t, TypeId recv); // registry signature type -> TypeId
    TypeId class_type_of(const ClassInfo& c); // self type incl. own params

    // ---- scopes ----
    enum class MoveState { available, moved, maybe_moved };
    struct Local {
        TypeId type;
        bool mut;
        bool borrowed;
        bool inout = false;
        MoveState move = MoveState::available;
    };
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }
    void declare(const std::string& name, TypeId t, bool mut, uint32_t line,
                 uint32_t col, bool borrowed = false);
    const Local* find_local(const std::string& name) const;
    Local* find_local_mut(const std::string& name);
    int local_scope_index(const std::string& name) const;
    void merge_move_states(const std::vector<std::map<std::string, Local>>& a,
                           const std::vector<std::map<std::string, Local>>& b);

    void error_at(uint32_t line, uint32_t col, std::string msg);

    // ---- state ----
    const Program& prog_;
    HirProgram hir_;
    TypePool& pool_;
    std::vector<CheckError> errors_;

    // keys are package-qualified names ("util.User"; root/builtins plain)
    std::map<std::string, ClassInfo> classes_;
    std::map<std::string, EnumInfo> enums_;
    std::map<std::string, TypeId> top_fns_;
    std::map<std::string, const FnDecl*> top_fn_decls_;
    std::set<std::string> builtin_generic_classes_; // List/Map/OrderedMap + thread containers

    std::map<std::string, std::string> prefix_by_path_; // import path -> pkg prefix

    std::vector<std::map<std::string, Local>> scopes_;

    // current context
    std::string cur_pkg_;  // prefix of the package being checked ("" = root)
    std::string cur_file_;
    std::map<std::string, std::string> pkg_paths_; // current file: binding -> import path
    ClassInfo* cur_class_ = nullptr;
    EnumInfo* cur_enum_ = nullptr;
    TypeId cur_ret_ = nullptr;
    bool cur_has_self_ = false;
    bool in_init_body_ = false; // super.init(...) is only meaningful here
    std::set<std::string> cur_type_params_;
    std::map<std::string, std::set<std::string>> cur_type_bounds_;
    // block nesting inside the current fn body (1 = top level) — defer is a
    // function-exit hook and is only legal at depth 1
    int block_depth_ = 0;
    int literal_sign_ = 1;
    std::set<const Expr*> bad_integer_literals_;
    int take_floor_depth_ = -1; // outer locals cannot be moved in loops/closures
    int capture_floor_depth_ = -1;
    bool require_send_captures_ = false;
    std::set<std::string> bad_send_captures_;
    bool allow_inout_expr_ = false;
    std::set<std::string> bad_inout_captures_;
    bool in_defer_ = false;
    int unsafe_depth_ = 0;

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
