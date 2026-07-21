#pragma once

#include <vector>

#include "value.h"

namespace beans {

// The builtin registry: one row here + one C runtime function + one interp
// fn below = a new builtin method, typed in the checker and emitted native.
// Monomorphic signatures only — generic containers (List/Map/Option/...)
// stay hand-written in the three stages; this table is how the stdlib grows.
enum class BT : uint8_t {
    unit,
    i64,
    f64,
    dec,
    boolean, // C side: long long 0/1
    str,
    res_i64, // Result<int>: C side returns BRes {tag, val, msg}
    res_f64,
    res_dec,
    res_str,
};

struct BuiltinMethod {
    BT recv; // receiver kind: str (scalars stay inline in codegen)
    const char* name;
    std::vector<BT> params;
    BT ret;
    const char* sym; // C runtime symbol
    Value (*run)(Value& recv, std::vector<Value>& args); // interpreter body
};

const std::vector<BuiltinMethod>& builtin_methods();

} // namespace beans
