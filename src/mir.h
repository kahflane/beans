#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast.h"

namespace beans {

enum class MirOp {
    statement,
    borrow,
    move,
    retain,
    release,
    edge_drop,
    allocate,
    branch,
    jump,
    return_,
};

struct MirInstruction {
    MirOp op = MirOp::statement;
    const Stmt* stmt = nullptr;
    const Expr* expr = nullptr;
    std::string value;
};

struct MirBlock {
    size_t id = 0;
    std::vector<MirInstruction> instructions;
    std::vector<size_t> successors;
};

struct MirFunction {
    const FnDecl* source = nullptr;
    size_t entry = 0;
    std::vector<MirBlock> blocks;
};

// First control-flow MIR. It keeps source nodes while the lowering is young,
// but control edges and ownership actions are explicit and backend-neutral.
class MirProgram {
public:
    explicit MirProgram(const Program& program);

    const MirFunction* function(const FnDecl* source) const;
    bool match_subject_needs_pin(const Expr* match) const;
    bool can_move_before_overwrite(const Expr* read) const;

private:
    std::map<const FnDecl*, MirFunction> functions_;
    std::map<const Expr*, bool> match_pins_;
    std::map<const Expr*, bool> overwrite_moves_;
};

} // namespace beans
