#pragma once

#include <map>

#include "ast.h"
#include "mir.h"
#include "target.h"
#include "types.h"

namespace beans {

// Checked whole-program view of the AST. The checker owns and fills this once;
// the interpreter and native backend then read the same semantic types instead
// of rebuilding them from syntax.
class HirProgram {
public:
    explicit HirProgram(const Program& ast) : ast_(ast), mir_(ast) {}

    const Program& ast() const { return ast_; }
    TypePool& types() { return types_; }
    const TypePool& types() const { return types_; }
    const TargetLayout& target() const { return target_; }
    const MirProgram& mir() const { return mir_; }

    void set_type(const Expr* expr, TypeId type) {
        if (expr) expr_types_[expr] = type;
    }

    TypeId type_of(const Expr* expr) const {
        auto it = expr_types_.find(expr);
        return it == expr_types_.end() ? nullptr : it->second;
    }

    // Interpolation expressions are parsed into temporary ASTs while checking.
    // Remove those addresses before the trees are freed, or a later parser may
    // reuse an address and appear to have an unrelated checked type.
    void forget_expr_tree(const Expr* expr) { forget_expr(expr); }

private:
    void forget_pattern(const Pattern* pattern) {
        if (!pattern) return;
        forget_expr(pattern->lit.get());
        forget_expr(pattern->lit2.get());
        for (const PatPtr& alt : pattern->alts) forget_pattern(alt.get());
    }

    void forget_stmt(const Stmt* stmt) {
        if (!stmt) return;
        forget_expr(stmt->init.get());
        forget_expr(stmt->target.get());
        forget_expr(stmt->value.get());
        forget_expr(stmt->expr.get());
        forget_expr(stmt->cond.get());
        forget_expr(stmt->iterable.get());
        for (const StmtPtr& child : stmt->body) forget_stmt(child.get());
        for (const StmtPtr& child : stmt->else_body) forget_stmt(child.get());
    }

    void forget_expr(const Expr* expr) {
        if (!expr) return;
        expr_types_.erase(expr);
        forget_expr(expr->lhs.get());
        forget_expr(expr->rhs.get());
        forget_expr(expr->callee.get());
        forget_expr(expr->object.get());
        forget_expr(expr->index_expr.get());
        forget_expr(expr->cond.get());
        forget_expr(expr->then_e.get());
        forget_expr(expr->else_e.get());
        forget_expr(expr->subject.get());
        for (const ExprPtr& arg : expr->args) forget_expr(arg.get());
        for (const InitEntry& entry : expr->entries) {
            forget_expr(entry.key.get());
            forget_expr(entry.value.get());
        }
        for (const StmtPtr& stmt : expr->body) forget_stmt(stmt.get());
        for (const MatchArm& arm : expr->arms) {
            forget_pattern(arm.pat.get());
            forget_expr(arm.value.get());
            for (const StmtPtr& stmt : arm.body) forget_stmt(stmt.get());
        }
    }

    const Program& ast_;
    MirProgram mir_;
    TargetLayout target_ = TargetLayout::host();
    TypePool types_;
    std::map<const Expr*, TypeId> expr_types_;
};

} // namespace beans
