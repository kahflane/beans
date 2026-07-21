#include "builtins.h"

#include <cstdlib>
#include <string>

namespace beans {

namespace {

Value res_ok(Value v) {
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Result";
    x.en->variant = "ok";
    x.en->payload.push_back(std::move(v));
    return x;
}

// same shape Interp::make_err builds — Error is an instance with msg/kind
Value res_err(std::string msg) {
    Value e;
    e.k = Value::K::instance;
    e.inst = std::make_shared<InstanceVal>();
    e.inst->fields.emplace_back("msg", Value::of_str(std::move(msg)));
    e.inst->fields.emplace_back("kind", Value::of_str(""));
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Result";
    x.en->variant = "err";
    x.en->payload.push_back(std::move(e));
    return x;
}

Value str_len(Value& recv, std::vector<Value>&) {
    return Value::of_int(static_cast<int64_t>(recv.s->size()));
}

Value str_last(Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t n = args[0].i;
    if (n < 0) n = 0;
    size_t take = static_cast<size_t>(n) > s.size() ? s.size() : static_cast<size_t>(n);
    return Value::of_str(s.substr(s.size() - take));
}

Value str_contains(Value& recv, std::vector<Value>& args) {
    return Value::of_bool(recv.s->find(*args[0].s) != std::string::npos);
}

Value str_to_int(Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    const char* p = s.c_str();
    char* end = nullptr;
    long long v = std::strtoll(p, &end, 10);
    if (end == p || *end != '\0') {
        return res_err("can't read '" + s + "' as int");
    }
    return res_ok(Value::of_int(v));
}

} // namespace

const std::vector<BuiltinMethod>& builtin_methods() {
    static const std::vector<BuiltinMethod> table = {
        {BT::str, "len", {}, BT::i64, "beans_str_len", str_len},
        {BT::str, "last", {BT::i64}, BT::str, "beans_str_last", str_last},
        {BT::str, "contains", {BT::str}, BT::boolean, "beans_str_contains", str_contains},
        {BT::str, "to_int", {}, BT::res_i64, "beans_str_to_int", str_to_int},
    };
    return table;
}

} // namespace beans
