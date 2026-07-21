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

// Native backend v1: emits textual LLVM IR, compiled by clang.
// Covers the numeric/string/control-flow core of beans — int, float, bool,
// string (+interpolation), top-level functions, if/for/match-free code.
// Everything else reports "not in the native backend yet".
class CodeGen {
public:
    explicit CodeGen(const Module& mod);

    // returns the .ll text; empty on failure
    std::string generate();
    const std::vector<CGError>& errors() const { return errors_; }

    // the C runtime that every beans binary links against
    static const char* runtime_c();

private:
    struct Fn; // per-function emitter (in codegen.cpp)

    const Module& mod_;
    std::vector<CGError> errors_;

    std::string globals_;       // string constants
    int next_str_ = 0;

    friend struct Fn;
    std::string intern_string(const std::string& bytes);
    void error_at(uint32_t line, uint32_t col, std::string msg);
};

} // namespace beans
