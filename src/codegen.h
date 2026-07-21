#pragma once

#include <string>
#include <vector>

#include "ast.h"

namespace beans {

struct CGError {
    std::string msg;
    uint32_t line;
    uint32_t col;
};

// Native backend: emits textual LLVM IR, compiled by clang. Covers the whole
// language; symbols and class instantiations are keyed by package-qualified
// names, so multi-package programs link into one flat module.
class CodeGen {
public:
    explicit CodeGen(const Program& prog);

    // returns the .ll text; empty on failure
    std::string generate();
    const std::vector<CGError>& errors() const { return errors_; }

    // the C runtime that every beans binary links against
    static const char* runtime_c();

private:
    const Program& prog_;
    std::vector<CGError> errors_;

    void error_at(uint32_t line, uint32_t col, std::string msg);
};

} // namespace beans
