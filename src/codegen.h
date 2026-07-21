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

// Native backend: emits textual LLVM IR, compiled by clang.
// v2 covers classes (vtable dispatch, inheritance, statics, interface
// defaults), enums + match, Option/Result + ?, decimal, lists, strings with
// interpolation, and all control flow. Not yet native: closures/fn values,
// threads, maps, generic functions — those report a clear error and keep
// working under `beansc run`.
class CodeGen {
public:
    explicit CodeGen(const Module& mod);

    // returns the .ll text; empty on failure
    std::string generate();
    const std::vector<CGError>& errors() const { return errors_; }

    // the C runtime that every beans binary links against
    static const char* runtime_c();

private:
    const Module& mod_;
    std::vector<CGError> errors_;

    void error_at(uint32_t line, uint32_t col, std::string msg);
};

} // namespace beans
