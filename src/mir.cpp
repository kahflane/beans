#include "mir.h"

#include <string_view>

namespace beans {
namespace {

bool statements_mention(const std::vector<StmtPtr>& statements,
                        std::string_view name);
bool expression_mentions(const Expr* expr, std::string_view name);

void collect_ident_reads(const Expr* expr, std::string_view name,
                         std::vector<const Expr*>& out) {
    if (!expr) return;
    if (expr->kind == Expr::Kind::ident && expr->text == name) out.push_back(expr);
    collect_ident_reads(expr->lhs.get(), name, out);
    collect_ident_reads(expr->rhs.get(), name, out);
    collect_ident_reads(expr->callee.get(), name, out);
    collect_ident_reads(expr->object.get(), name, out);
    collect_ident_reads(expr->index_expr.get(), name, out);
    collect_ident_reads(expr->cond.get(), name, out);
    collect_ident_reads(expr->then_e.get(), name, out);
    collect_ident_reads(expr->else_e.get(), name, out);
    collect_ident_reads(expr->subject.get(), name, out);
    for (const ExprPtr& arg : expr->args) collect_ident_reads(arg.get(), name, out);
    for (const InitEntry& entry : expr->entries) {
        collect_ident_reads(entry.key.get(), name, out);
        collect_ident_reads(entry.value.get(), name, out);
    }
    for (const MatchArm& arm : expr->arms)
        collect_ident_reads(arm.value.get(), name, out);
}

void collect_statement_reads(const Stmt* statement, std::string_view name,
                             std::vector<const Expr*>& out) {
    if (!statement) return;
    collect_ident_reads(statement->init.get(), name, out);
    collect_ident_reads(statement->value.get(), name, out);
    collect_ident_reads(statement->expr.get(), name, out);
    collect_ident_reads(statement->cond.get(), name, out);
    collect_ident_reads(statement->iterable.get(), name, out);
}

void find_overwrite_moves(const std::vector<StmtPtr>& statements,
                          std::map<const Expr*, bool>& moves) {
    for (size_t i = 0; i + 1 < statements.size(); i++) {
        const Stmt* current = statements[i].get();
        const Stmt* next = statements[i + 1].get();
        if (!current->body.empty() || !current->else_body.empty() ||
            next->kind != Stmt::Kind::assign || next->op != TokenKind::assign ||
            !next->target || next->target->kind != Expr::Kind::ident)
            continue;
        std::string_view name = next->target->text;
        if (expression_mentions(next->value.get(), name)) continue;
        std::vector<const Expr*> reads;
        collect_statement_reads(current, name, reads);
        if (reads.size() == 1) moves[reads[0]] = true;
    }
    for (const StmtPtr& statement : statements) {
        find_overwrite_moves(statement->body, moves);
        find_overwrite_moves(statement->else_body, moves);
        if (statement->expr && statement->expr->kind == Expr::Kind::match_expr) {
            for (const MatchArm& arm : statement->expr->arms)
                find_overwrite_moves(arm.body, moves);
        }
    }
}

bool expression_mentions(const Expr* expr, std::string_view name) {
    if (!expr) return false;
    if (expr->kind == Expr::Kind::ident && expr->text == name) return true;
    if (expr->kind == Expr::Kind::self_ref && name == "self") return true;
    // Interpolation is parsed after checking. Treat it as unknown because a
    // hidden call may mutate the owner whose field is being matched.
    if (expr->kind == Expr::Kind::string_lit &&
        expr->text.find('{') != std::string_view::npos)
        return true;
    if (expression_mentions(expr->lhs.get(), name) ||
        expression_mentions(expr->rhs.get(), name) ||
        expression_mentions(expr->callee.get(), name) ||
        expression_mentions(expr->object.get(), name) ||
        expression_mentions(expr->index_expr.get(), name) ||
        expression_mentions(expr->cond.get(), name) ||
        expression_mentions(expr->then_e.get(), name) ||
        expression_mentions(expr->else_e.get(), name) ||
        expression_mentions(expr->subject.get(), name))
        return true;
    for (const ExprPtr& arg : expr->args)
        if (expression_mentions(arg.get(), name)) return true;
    for (const InitEntry& entry : expr->entries) {
        if (expression_mentions(entry.key.get(), name) ||
            expression_mentions(entry.value.get(), name))
            return true;
    }
    if (statements_mention(expr->body, name)) return true;
    for (const MatchArm& arm : expr->arms) {
        if (expression_mentions(arm.value.get(), name) ||
            statements_mention(arm.body, name))
            return true;
    }
    return false;
}

bool statement_mentions(const Stmt* statement, std::string_view name) {
    if (!statement) return false;
    return expression_mentions(statement->init.get(), name) ||
           expression_mentions(statement->target.get(), name) ||
           expression_mentions(statement->value.get(), name) ||
           expression_mentions(statement->expr.get(), name) ||
           expression_mentions(statement->cond.get(), name) ||
           expression_mentions(statement->iterable.get(), name) ||
           statements_mention(statement->body, name) ||
           statements_mention(statement->else_body, name);
}

bool statements_mention(const std::vector<StmtPtr>& statements,
                        std::string_view name) {
    for (const StmtPtr& statement : statements)
        if (statement_mentions(statement.get(), name)) return true;
    return false;
}

bool needs_match_pin(const Expr* match) {
    const Expr* base = match ? match->subject.get() : nullptr;
    if (!base || (base->kind != Expr::Kind::field &&
                  base->kind != Expr::Kind::index))
        return true;
    while (base && (base->kind == Expr::Kind::field ||
                    base->kind == Expr::Kind::index))
        base = base->object.get();
    std::string_view root;
    if (base && base->kind == Expr::Kind::ident) root = base->text;
    else if (base && base->kind == Expr::Kind::self_ref) root = "self";
    else return true;
    for (const MatchArm& arm : match->arms) {
        if (expression_mentions(arm.value.get(), root) ||
            statements_mention(arm.body, root))
            return true;
    }
    return false;
}

class Builder {
public:
    Builder(const FnDecl& source, std::map<const Expr*, bool>& match_pins,
            std::map<const Expr*, bool>& overwrite_moves)
        : match_pins_(match_pins) {
        function_.source = &source;
        function_.entry = block();
        find_overwrite_moves(source.body, overwrite_moves);
        lower_statements(source.body, function_.entry);
    }

    MirFunction take() { return std::move(function_); }

private:
    size_t block() {
        size_t id = function_.blocks.size();
        function_.blocks.push_back({id, {}, {}});
        return id;
    }

    void emit(size_t id, MirOp op, const Stmt* stmt = nullptr,
              const Expr* expr = nullptr, std::string value = {}) {
        function_.blocks[id].instructions.push_back(
            {op, stmt, expr, std::move(value)});
    }

    void edge(size_t from, size_t to) {
        function_.blocks[from].successors.push_back(to);
    }

    void scan_expr(const Expr* expr, size_t id) {
        if (!expr) return;
        switch (expr->kind) {
            case Expr::Kind::ident:
                emit(id, MirOp::borrow, nullptr, expr, std::string(expr->text));
                break;
            case Expr::Kind::self_ref:
                emit(id, MirOp::borrow, nullptr, expr, "self");
                break;
            case Expr::Kind::unary:
                if (expr->op == TokenKind::kw_move && expr->rhs &&
                    expr->rhs->kind == Expr::Kind::ident) {
                    emit(id, MirOp::move, nullptr, expr,
                         std::string(expr->rhs->text));
                    return;
                }
                break;
            case Expr::Kind::list_lit:
            case Expr::Kind::init:
            case Expr::Kind::new_:
            case Expr::Kind::closure:
                emit(id, MirOp::allocate, nullptr, expr);
                break;
            case Expr::Kind::match_expr: {
                bool pin = needs_match_pin(expr);
                match_pins_[expr] = pin;
                emit(id, pin ? MirOp::retain : MirOp::borrow, nullptr,
                     expr->subject.get(), "match subject");
                if (pin)
                    emit(id, MirOp::release, nullptr, expr->subject.get(),
                         "match subject");
                break;
            }
            default:
                break;
        }
        scan_expr(expr->lhs.get(), id);
        scan_expr(expr->rhs.get(), id);
        scan_expr(expr->callee.get(), id);
        scan_expr(expr->object.get(), id);
        scan_expr(expr->index_expr.get(), id);
        scan_expr(expr->cond.get(), id);
        scan_expr(expr->then_e.get(), id);
        scan_expr(expr->else_e.get(), id);
        scan_expr(expr->subject.get(), id);
        for (const ExprPtr& arg : expr->args) scan_expr(arg.get(), id);
        for (const InitEntry& entry : expr->entries) {
            scan_expr(entry.key.get(), id);
            scan_expr(entry.value.get(), id);
        }
        for (const MatchArm& arm : expr->arms) scan_expr(arm.value.get(), id);
    }

    size_t lower_statements(const std::vector<StmtPtr>& statements, size_t current) {
        for (const StmtPtr& owned : statements) {
            const Stmt* statement = owned.get();
            emit(current, MirOp::statement, statement);
            scan_expr(statement->init.get(), current);
            scan_expr(statement->target.get(), current);
            scan_expr(statement->value.get(), current);
            scan_expr(statement->expr.get(), current);
            scan_expr(statement->cond.get(), current);
            scan_expr(statement->iterable.get(), current);
            if (statement->kind == Stmt::Kind::let_ && statement->init)
                emit(current, MirOp::move, statement, statement->init.get(),
                     statement->name);
            if (statement->kind == Stmt::Kind::assign && statement->target &&
                (statement->target->kind == Expr::Kind::field ||
                 statement->target->kind == Expr::Kind::index))
                emit(current, MirOp::edge_drop, statement,
                     statement->target.get());

            if (statement->kind == Stmt::Kind::ret) {
                emit(current, MirOp::return_, statement, statement->expr.get());
                return current;
            }
            if (statement->kind == Stmt::Kind::if_) {
                size_t yes = block(), no = block(), join = block();
                emit(current, MirOp::branch, statement, statement->cond.get());
                edge(current, yes);
                edge(current, no);
                size_t yes_end = lower_statements(statement->body, yes);
                emit(yes_end, MirOp::jump, statement);
                edge(yes_end, join);
                size_t no_end = lower_statements(statement->else_body, no);
                emit(no_end, MirOp::jump, statement);
                edge(no_end, join);
                current = join;
            } else if (statement->kind == Stmt::Kind::for_ever ||
                       statement->kind == Stmt::Kind::for_while ||
                       statement->kind == Stmt::Kind::for_in) {
                size_t head = block(), body = block(), done = block();
                emit(current, MirOp::jump, statement);
                edge(current, head);
                emit(head, MirOp::branch, statement, statement->cond.get());
                edge(head, body);
                edge(head, done);
                size_t body_end = lower_statements(statement->body, body);
                emit(body_end, MirOp::jump, statement);
                edge(body_end, head);
                current = done;
            }
        }
        return current;
    }

    MirFunction function_;
    std::map<const Expr*, bool>& match_pins_;
};

} // namespace

MirProgram::MirProgram(const Program& program) {
    auto add = [&](const FnDecl& function) {
        Builder builder(function, match_pins_, overwrite_moves_);
        functions_.emplace(&function, builder.take());
    };
    for (const auto& package : program.packages) {
        for (const auto& file : package->files) {
            for (const FnDecl& function : file->mod.fns) add(function);
            for (const ClassDecl& cls : file->mod.classes)
                for (const FnDecl& method : cls.methods) add(method);
            for (const EnumDecl& enumeration : file->mod.enums)
                for (const FnDecl& method : enumeration.methods) add(method);
        }
    }
}

const MirFunction* MirProgram::function(const FnDecl* source) const {
    auto it = functions_.find(source);
    return it == functions_.end() ? nullptr : &it->second;
}

bool MirProgram::match_subject_needs_pin(const Expr* match) const {
    auto it = match_pins_.find(match);
    return it == match_pins_.end() || it->second;
}

bool MirProgram::can_move_before_overwrite(const Expr* read) const {
    auto it = overwrite_moves_.find(read);
    return it != overwrite_moves_.end() && it->second;
}

} // namespace beans
