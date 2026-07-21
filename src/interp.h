#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ast.h"
#include "builtins.h"
#include "lexer.h"
#include "value.h"

namespace beans {

// Thrown for beans runtime errors (index out of range, expect() failure, ...).
struct BeansPanic {
    std::string msg;
    uint32_t line = 0, col = 0;
};

// live worker-thread count (parallel to the native runtime's cc_threads). A
// File/MMap closed while any worker runs defers its real fd/munmap release
// until the handle's last ref drops, so the fd number is never reused for a
// different file mid-op on another thread.
void beans_threads_inc();
void beans_threads_dec();
bool beans_threads_live();

// Tree-walking interpreter. Assumes the program already passed the checker —
// it does not re-validate types, it just runs. Global tables are keyed by
// package-qualified names; the checker's AST annotations (Expr::resolved,
// TypeRef::resolved) carry cross-package references, and a per-thread
// current-package prefix resolves plain names (needed for string-interpolation
// segments, which are parsed here and never saw the checker).
class Interp {
public:
    explicit Interp(const Program& prog);

    // find fn main and run it. returns the process exit code.
    int run();

private:
    // control-flow signals
    struct ReturnSignal { Value v; };
    struct BreakSignal {};
    struct ContinueSignal {};

    // number-literal hint so decimal literals parse exactly from source text
    enum class NumHint { none, dec, flt };
    struct Hint {
        const TypeRef* tref;
        NumHint num;
        Hint() : tref(nullptr), num(NumHint::none) {}
        static Hint of(const TypeRef* t);
        static Hint decimal() { Hint h; h.num = NumHint::dec; return h; }
        static Hint floating() { Hint h; h.num = NumHint::flt; return h; }
        bool wants_dec() const;
        bool wants_float() const;
        const TypeRef* arg(size_t i) const;
    };

    // execution; pkg = package prefix the function's body lives in
    Value call_fn(const FnDecl* fn, Value* self, std::vector<Value> args,
                  const std::string& pkg);
    Value call_closure(const ClosureVal& c, std::vector<Value> args);
    void exec_block(const std::vector<StmtPtr>& body, std::shared_ptr<Env> env);
    void exec_stmt(const Stmt* s, std::shared_ptr<Env>& env);

    // Evaluate one beans AST node. (Named like every interpreter textbook's
    // eval — it walks our own type-checked AST only; it never executes
    // arbitrary strings or host code, so the usual eval() risk doesn't apply.)
    Value eval(const Expr* e, std::shared_ptr<Env>& env, Hint hint = {});
    Value eval_call(const Expr* e, std::shared_ptr<Env>& env, Hint hint);
    Value eval_binary(const Expr* e, std::shared_ptr<Env>& env);
    Value eval_init(const Expr* e, std::shared_ptr<Env>& env, Hint hint);
    Value eval_match(const Expr* e, std::shared_ptr<Env>& env, Hint hint);
    Value eval_builtin_method(const Expr* e, Value& recv, const std::string& name,
                              std::vector<Value>& args);
    std::vector<Value> eval_method_args(const Expr* e, std::shared_ptr<Env>& env,
                                        const Value& recv, const std::string& name);
    static Hint hint_of_bt(BT p);
    static Hint map_key_hint(const MapVal& m);
    static std::string display_scalar(const Value& v);
    Value eval_string(const Expr* e, std::shared_ptr<Env>& env);

    bool match_pattern(const Pattern* p, const Value& v, Env& bind_env,
                       std::shared_ptr<Env>& outer);

    // assignment targets
    void assign_to(const Expr* target, Value v, std::shared_ptr<Env>& env);
    Value* lvalue_slot(const Expr* target, std::shared_ptr<Env>& env);

    // helpers
    Value make_instance(const ClassDecl* cls,
                        const std::vector<InitEntry>& entries,
                        std::shared_ptr<Env>& env);
    const FnDecl* find_method(const ClassDecl* cls, const std::string& name,
                              const ClassDecl** owner = nullptr) const;
    const ClassDecl* find_class(const std::string& name) const;
    bool class_is(const ClassDecl* cls, const std::string& super) const;
    void collect_fields(const ClassDecl* cls, std::vector<const FieldDecl*>& out) const;

    // package-aware resolution: annotation first, else the running package
    std::string qual(const std::string& name) const;
    static std::string pkg_of(const std::string& qualname);
    const ClassDecl* resolve_class(const Expr* annotated, const std::string& name) const;
    const EnumDecl* resolve_enum(const Expr* annotated, const std::string& name) const;
    // import binding of the running package -> import path ("" if none)
    std::string binding_path(const std::string& binding) const;

    Value coerce_arg(Value v, const TypeRef* want);
    static bool value_eq(const Value& a, const Value& b);
    // map index plumbing — hashing must agree with value_eq exactly
    static uint64_t value_hash(const Value& v);
    static size_t map_find(MapVal& m, const Value& key, uint64_t& h,
                           size_t* slot_out = nullptr);
    static void map_append(MapVal& m, uint64_t h, Value key, Value val);
    static void map_set(MapVal& m, Value key, Value val);
    static bool map_remove_key(MapVal& m, const Value& key);
    static void map_reindex(MapVal& m);
    static std::string display(const Value& v);
    [[noreturn]] void panic(const Expr* e, std::string msg);

    Value make_err(std::string msg);
    Value some(Value v);
    Value none();

    // interpolated strings, split and parsed once
    struct StrPart {
        std::string text;    // literal piece (already unescaped)
        ExprPtr expr;        // or an expression piece
        FmtSpec spec;        // "{x:8.2}" — applied after rendering
        std::shared_ptr<std::string> src; // owns the segment text the expr's
                                          // string_views point into
    };
    const std::vector<StrPart>& string_parts(const Expr* e);

    // keyed by package-qualified names ("util.User"; root plain)
    std::map<std::string, const ClassDecl*> classes_;
    std::map<std::string, const EnumDecl*> enums_;
    std::map<std::string, const FnDecl*> fns_;
    // per package prefix: import binding -> import path (merged over its files)
    std::map<std::string, std::map<std::string, std::string>> pkg_imports_;
    std::map<std::string, std::string> prefix_by_path_;

    std::mutex str_cache_mu_;
    std::map<const Expr*, std::vector<StrPart>> str_cache_;
};

} // namespace beans
