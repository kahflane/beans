#pragma once

#include <string>
#include <vector>

#include "hir.h"

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
    explicit CodeGen(const HirProgram& hir);

    // returns the .ll text; empty on failure
    std::string generate();
    const std::vector<CGError>& errors() const { return errors_; }
    const std::string& ffi_c() const { return ffi_c_; }

private:
    const HirProgram& hir_;
    const Program& prog_;
    std::vector<CGError> errors_;
    std::string ffi_c_;

    void error_at(uint32_t line, uint32_t col, std::string msg);
};

} // namespace beans
