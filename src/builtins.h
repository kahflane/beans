#pragma once

#include <cstdint>
#include <vector>

#include "value.h"

namespace beans {

// The builtin registry: one row here + one C runtime function + one interp
// fn in builtins.cpp = a new builtin, typed in the checker and emitted
// native. Monomorphic signatures only — generic containers (List/Map/...)
// stay hand-written in the three stages; this table is how the stdlib grows.
enum class BT : uint8_t {
    unit,
    i64,
    f64,
    dec,
    boolean,   // C side: long long 0/1
    str,
    bytes,
    file,
    mmap,
    self_recv, // return: the receiver itself, borrowed — chaining mutators
    opt_i64,   // Option<...>: C side returns BOpt {val, has}
    opt_str,
    list_str,  // List<string>: C side returns a built list
    res_i64,   // Result<...>: C side returns BRes {val, err}; err is null or
    res_f64,   // a ready-made Error object (msg + kind already set)
    res_dec,
    res_str,
    res_bool,
    res_bytes,
    res_file,
    res_mmap,
    res_list_str,
};

struct BuiltinMethod {
    BT recv; // receiver kind: str or bytes (scalars stay inline in codegen)
    const char* name;
    std::vector<BT> params;
    BT ret;
    const char* sym; // C runtime symbol
    // C fn takes trailing (i64 line, i64 col) and may call beans_panic;
    // the interp side throws BeansPanic with the same message
    bool panics;
    Value (*run)(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args);
};

struct BuiltinStatic {
    const char* cls; // "Bytes", "File", "Dir"
    const char* name;
    std::vector<BT> params;
    BT ret;
    const char* sym;
    bool panics;
    Value (*run)(uint32_t line, uint32_t col, std::vector<Value>& args);
};

// module-level std functions ("std.os" and friends); println/eprintln stay
// special — they take any printable
struct BuiltinFn {
    const char* module; // import path, e.g. "std.os"
    const char* name;
    std::vector<BT> params;
    BT ret;
    const char* sym;
    bool panics;
    Value (*run)(uint32_t line, uint32_t col, std::vector<Value>& args);
};

const std::vector<BuiltinMethod>& builtin_methods();
const std::vector<BuiltinStatic>& builtin_statics();
const std::vector<BuiltinFn>& builtin_fns();

// `beansc run` forwards program arguments here for std.os args()
void set_program_args(std::vector<std::string> args);

} // namespace beans
