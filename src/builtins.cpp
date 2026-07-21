#include "builtins.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "interp.h" // BeansPanic — builtin rows panic with the same messages
                    // the C runtime prints; the two must stay byte-identical

namespace beans {

namespace {

[[noreturn]] void bpanic(uint32_t line, uint32_t col, std::string msg) {
    throw BeansPanic{std::move(msg), line, col};
}

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

Value opt_some(Value v) {
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Option";
    x.en->variant = "some";
    x.en->payload.push_back(std::move(v));
    return x;
}

Value opt_none() {
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Option";
    x.en->variant = "none";
    return x;
}

Value str_list(std::vector<std::string> parts) {
    Value v;
    v.k = Value::K::list;
    v.list = std::make_shared<ListVal>();
    for (std::string& p : parts) v.list->items.push_back(Value::of_str(std::move(p)));
    return v;
}

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ---- string methods ---------------------------------------------------------

Value str_len(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    return Value::of_int(static_cast<int64_t>(recv.s->size()));
}

Value str_is_empty(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    return Value::of_bool(recv.s->empty());
}

Value str_last(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t n = args[0].i;
    if (n < 0) n = 0;
    size_t take = static_cast<size_t>(n) > s.size() ? s.size() : static_cast<size_t>(n);
    return Value::of_str(s.substr(s.size() - take));
}

Value str_first(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t n = args[0].i;
    if (n < 0) n = 0;
    size_t take = static_cast<size_t>(n) > s.size() ? s.size() : static_cast<size_t>(n);
    return Value::of_str(s.substr(0, take));
}

Value str_contains(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    return Value::of_bool(recv.s->find(*args[0].s) != std::string::npos);
}

Value str_starts_with(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    return Value::of_bool(recv.s->rfind(*args[0].s, 0) == 0);
}

Value str_ends_with(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    const std::string& p = *args[0].s;
    return Value::of_bool(p.size() <= s.size() &&
                          memcmp(s.data() + s.size() - p.size(), p.data(), p.size()) == 0);
}

Value str_find(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    size_t i = recv.s->find(*args[0].s);
    if (i == std::string::npos) return opt_none();
    return opt_some(Value::of_int(static_cast<int64_t>(i)));
}

Value str_rfind(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    size_t i = recv.s->rfind(*args[0].s);
    if (i == std::string::npos) return opt_none();
    return opt_some(Value::of_int(static_cast<int64_t>(i)));
}

Value str_slice(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t from = args[0].i, to = args[1].i;
    if (from < 0 || to < from || static_cast<size_t>(to) > s.size()) {
        bpanic(line, col, "slice " + std::to_string(from) + ".." + std::to_string(to) +
                              " out of range (len " + std::to_string(s.size()) + ")");
    }
    return Value::of_str(s.substr(static_cast<size_t>(from), static_cast<size_t>(to - from)));
}

Value str_byte_at(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t i = args[0].i;
    if (i < 0 || static_cast<size_t>(i) >= s.size()) {
        bpanic(line, col, "byte index " + std::to_string(i) + " out of range (len " +
                              std::to_string(s.size()) + ")");
    }
    return Value::of_int(static_cast<uint8_t>(s[static_cast<size_t>(i)]));
}

Value str_trim(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) b++;
    while (e > b && is_ws(s[e - 1])) e--;
    return Value::of_str(s.substr(b, e - b));
}

Value str_trim_start(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    size_t b = 0;
    while (b < s.size() && is_ws(s[b])) b++;
    return Value::of_str(s.substr(b));
}

Value str_trim_end(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    size_t e = s.size();
    while (e > 0 && is_ws(s[e - 1])) e--;
    return Value::of_str(s.substr(0, e));
}

Value str_to_upper(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    std::string s = *recv.s;
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return Value::of_str(std::move(s));
}

Value str_to_lower(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    std::string s = *recv.s;
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return Value::of_str(std::move(s));
}

Value str_replace(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    const std::string& old = *args[0].s;
    const std::string& sub = *args[1].s;
    if (old.empty()) return Value::of_str(s); // replacing nothing changes nothing
    std::string out;
    size_t i = 0;
    while (true) {
        size_t j = s.find(old, i);
        if (j == std::string::npos) break;
        out.append(s, i, j - i);
        out += sub;
        i = j + old.size();
    }
    out.append(s, i, s.size() - i);
    return Value::of_str(std::move(out));
}

Value str_repeat(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    int64_t n = args[0].i;
    if (n < 0) bpanic(line, col, "negative repeat count " + std::to_string(n));
    std::string out;
    out.reserve(recv.s->size() * static_cast<size_t>(n));
    for (int64_t i = 0; i < n; i++) out += *recv.s;
    return Value::of_str(std::move(out));
}

Value str_split(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    const std::string& s = *recv.s;
    const std::string& sep = *args[0].s;
    std::vector<std::string> parts;
    if (sep.empty()) {
        parts.push_back(s); // no separator: the whole string, one piece
    } else {
        size_t i = 0;
        while (true) {
            size_t j = s.find(sep, i);
            if (j == std::string::npos) break;
            parts.push_back(s.substr(i, j - i));
            i = j + sep.size();
        }
        parts.push_back(s.substr(i));
    }
    return str_list(std::move(parts));
}

Value str_lines(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    std::vector<std::string> parts;
    size_t i = 0;
    while (true) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) break;
        parts.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    // a trailing newline doesn't make an empty final line
    if (i < s.size()) parts.push_back(s.substr(i));
    return str_list(std::move(parts));
}

Value str_to_int(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    const char* p = s.c_str();
    char* end = nullptr;
    long long v = std::strtoll(p, &end, 10);
    if (end == p || *end != '\0') {
        return res_err("can't read '" + s + "' as int");
    }
    return res_ok(Value::of_int(v));
}

Value str_to_float(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    const char* p = s.c_str();
    char* end = nullptr;
    double v = std::strtod(p, &end);
    if (end == p || *end != '\0') {
        return res_err("can't read '" + s + "' as float");
    }
    return res_ok(Value::of_float(v));
}

// shared with the C runtime: [+-]? digits with '_', one optional '.',
// one optional e/E exponent with its own sign and >=1 digit
bool dec_valid(const char* s) {
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') i++;
    int digits = 0, dot = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { digits++; continue; }
        if (c == '_') continue;
        if (c == '.' && !dot) { dot = 1; continue; }
        if ((c == 'e' || c == 'E') && digits) {
            i++;
            if (s[i] == '+' || s[i] == '-') i++;
            if (!(s[i] >= '0' && s[i] <= '9')) return false;
            while (s[i] >= '0' && s[i] <= '9') i++;
            return s[i] == '\0';
        }
        return false;
    }
    return digits > 0;
}

Value str_to_decimal(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    if (!dec_valid(s.c_str())) {
        return res_err("can't read '" + s + "' as decimal");
    }
    return res_ok(Value::of_dec(Decimal::parse(s)));
}

// ---- Bytes ------------------------------------------------------------------

[[noreturn]] void byte_oob(uint32_t line, uint32_t col, int64_t i, size_t len) {
    bpanic(line, col, "byte index " + std::to_string(i) + " out of range (len " +
                          std::to_string(len) + ")");
}

Value bytes_len(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    return Value::of_int(static_cast<int64_t>(recv.bytes->data.size()));
}

Value bytes_resize(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    int64_t n = args[0].i;
    if (n < 0) bpanic(line, col, "negative size " + std::to_string(n));
    recv.bytes->data.resize(static_cast<size_t>(n), 0);
    return recv;
}

Value bytes_fill(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    for (uint8_t& b : recv.bytes->data) b = static_cast<uint8_t>(args[0].i & 255);
    return recv;
}

Value bytes_get(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    auto& d = recv.bytes->data;
    int64_t i = args[0].i;
    if (i < 0 || static_cast<size_t>(i) >= d.size()) byte_oob(line, col, i, d.size());
    return Value::of_int(d[static_cast<size_t>(i)]);
}

Value bytes_set(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    auto& d = recv.bytes->data;
    int64_t i = args[0].i;
    if (i < 0 || static_cast<size_t>(i) >= d.size()) byte_oob(line, col, i, d.size());
    d[static_cast<size_t>(i)] = static_cast<uint8_t>(args[1].i & 255);
    return recv;
}

[[noreturn]] void width_oob(uint32_t line, uint32_t col, const char* what, const char* op,
                            int64_t pos, size_t len) {
    bpanic(line, col, std::string(what) + " " + op + " at " + std::to_string(pos) +
                          " out of range (len " + std::to_string(len) + ")");
}

Value bytes_get_w(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args,
                  const char* what, size_t w, bool sign) {
    auto& d = recv.bytes->data;
    int64_t pos = args[0].i;
    if (pos < 0 || static_cast<size_t>(pos) + w > d.size()) {
        width_oob(line, col, what, "read", pos, d.size());
    }
    uint64_t v = 0;
    memcpy(&v, d.data() + pos, w); // little-endian hosts only, documented
    if (sign && w == 8) return Value::of_int(static_cast<int64_t>(v));
    return Value::of_int(static_cast<int64_t>(v));
}

Value bytes_put_w(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args,
                  const char* what, size_t w) {
    auto& d = recv.bytes->data;
    int64_t pos = args[0].i;
    if (pos < 0 || static_cast<size_t>(pos) + w > d.size()) {
        width_oob(line, col, what, "write", pos, d.size());
    }
    uint64_t v = static_cast<uint64_t>(args[1].i);
    memcpy(d.data() + pos, &v, w);
    return recv;
}

Value bytes_get_u8(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_get_w(l, c, r, a, "u8", 1, false); }
Value bytes_get_u16(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_get_w(l, c, r, a, "u16", 2, false); }
Value bytes_get_u32(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_get_w(l, c, r, a, "u32", 4, false); }
Value bytes_get_u64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_get_w(l, c, r, a, "u64", 8, false); }
Value bytes_get_i64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_get_w(l, c, r, a, "i64", 8, true); }
Value bytes_put_u8(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_put_w(l, c, r, a, "u8", 1); }
Value bytes_put_u16(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_put_w(l, c, r, a, "u16", 2); }
Value bytes_put_u32(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_put_w(l, c, r, a, "u32", 4); }
Value bytes_put_u64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_put_w(l, c, r, a, "u64", 8); }
Value bytes_put_i64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return bytes_put_w(l, c, r, a, "i64", 8); }

Value bytes_slice(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    auto& d = recv.bytes->data;
    int64_t from = args[0].i, to = args[1].i;
    if (from < 0 || to < from || static_cast<size_t>(to) > d.size()) {
        bpanic(line, col, "slice " + std::to_string(from) + ".." + std::to_string(to) +
                              " out of range (len " + std::to_string(d.size()) + ")");
    }
    Value v;
    v.k = Value::K::bytes;
    v.bytes = std::make_shared<BytesVal>();
    v.bytes->data.assign(d.begin() + from, d.begin() + to);
    return v;
}

Value bytes_copy_from(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    auto& d = recv.bytes->data;
    auto& src = args[0].bytes->data;
    int64_t at = args[1].i;
    if (at < 0 || static_cast<size_t>(at) + src.size() > d.size()) {
        bpanic(line, col, "copy of " + std::to_string(src.size()) + " bytes at " +
                              std::to_string(at) + " out of range (len " +
                              std::to_string(d.size()) + ")");
    }
    memcpy(d.data() + at, src.data(), src.size());
    return recv;
}

Value bytes_append(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    auto& src = args[0].bytes->data;
    recv.bytes->data.insert(recv.bytes->data.end(), src.begin(), src.end());
    return recv;
}

Value bytes_append_str(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    const std::string& s = *args[0].s;
    recv.bytes->data.insert(recv.bytes->data.end(), s.begin(), s.end());
    return recv;
}

Value bytes_to_string(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    auto& d = recv.bytes->data;
    size_t n = 0;
    while (n < d.size() && d[n] != 0) n++; // strings are text: stop at NUL
    return Value::of_str(std::string(reinterpret_cast<const char*>(d.data()), n));
}

// ---- statics ---------------------------------------------------------------

Value bytes_new(uint32_t line, uint32_t col, std::vector<Value>& args) {
    int64_t n = args[0].i;
    if (n < 0) bpanic(line, col, "negative size " + std::to_string(n));
    Value v;
    v.k = Value::K::bytes;
    v.bytes = std::make_shared<BytesVal>();
    v.bytes->data.assign(static_cast<size_t>(n), 0);
    return v;
}

Value bytes_from(uint32_t, uint32_t, std::vector<Value>& args) {
    const std::string& s = *args[0].s;
    Value v;
    v.k = Value::K::bytes;
    v.bytes = std::make_shared<BytesVal>();
    v.bytes->data.assign(s.begin(), s.end());
    return v;
}

} // namespace

const std::vector<BuiltinMethod>& builtin_methods() {
    static const std::vector<BuiltinMethod> table = {
        // strings
        {BT::str, "len", {}, BT::i64, "beans_str_len", false, str_len},
        {BT::str, "is_empty", {}, BT::boolean, "beans_str_is_empty", false, str_is_empty},
        {BT::str, "last", {BT::i64}, BT::str, "beans_str_last", false, str_last},
        {BT::str, "first", {BT::i64}, BT::str, "beans_str_first", false, str_first},
        {BT::str, "contains", {BT::str}, BT::boolean, "beans_str_contains", false, str_contains},
        {BT::str, "starts_with", {BT::str}, BT::boolean, "beans_str_starts_with", false, str_starts_with},
        {BT::str, "ends_with", {BT::str}, BT::boolean, "beans_str_ends_with", false, str_ends_with},
        {BT::str, "find", {BT::str}, BT::opt_i64, "beans_str_find", false, str_find},
        {BT::str, "rfind", {BT::str}, BT::opt_i64, "beans_str_rfind", false, str_rfind},
        {BT::str, "slice", {BT::i64, BT::i64}, BT::str, "beans_str_slice", true, str_slice},
        {BT::str, "byte_at", {BT::i64}, BT::i64, "beans_str_byte_at", true, str_byte_at},
        {BT::str, "trim", {}, BT::str, "beans_str_trim", false, str_trim},
        {BT::str, "trim_start", {}, BT::str, "beans_str_trim_start", false, str_trim_start},
        {BT::str, "trim_end", {}, BT::str, "beans_str_trim_end", false, str_trim_end},
        {BT::str, "to_upper", {}, BT::str, "beans_str_to_upper", false, str_to_upper},
        {BT::str, "to_lower", {}, BT::str, "beans_str_to_lower", false, str_to_lower},
        {BT::str, "replace", {BT::str, BT::str}, BT::str, "beans_str_replace", false, str_replace},
        {BT::str, "repeat", {BT::i64}, BT::str, "beans_str_repeat", true, str_repeat},
        {BT::str, "split", {BT::str}, BT::list_str, "beans_str_split", false, str_split},
        {BT::str, "lines", {}, BT::list_str, "beans_str_lines", false, str_lines},
        {BT::str, "to_int", {}, BT::res_i64, "beans_str_to_int", false, str_to_int},
        {BT::str, "to_float", {}, BT::res_f64, "beans_str_to_float", false, str_to_float},
        {BT::str, "to_decimal", {}, BT::res_dec, "beans_str_to_decimal", false, str_to_decimal},
        // Bytes
        {BT::bytes, "len", {}, BT::i64, "beans_bytes_len", false, bytes_len},
        {BT::bytes, "resize", {BT::i64}, BT::self_recv, "beans_bytes_resize", true, bytes_resize},
        {BT::bytes, "fill", {BT::i64}, BT::self_recv, "beans_bytes_fill", false, bytes_fill},
        {BT::bytes, "get", {BT::i64}, BT::i64, "beans_bytes_get", true, bytes_get},
        {BT::bytes, "set", {BT::i64, BT::i64}, BT::self_recv, "beans_bytes_set", true, bytes_set},
        {BT::bytes, "get_u8", {BT::i64}, BT::i64, "beans_bytes_get_u8", true, bytes_get_u8},
        {BT::bytes, "get_u16", {BT::i64}, BT::i64, "beans_bytes_get_u16", true, bytes_get_u16},
        {BT::bytes, "get_u32", {BT::i64}, BT::i64, "beans_bytes_get_u32", true, bytes_get_u32},
        {BT::bytes, "get_u64", {BT::i64}, BT::i64, "beans_bytes_get_u64", true, bytes_get_u64},
        {BT::bytes, "get_i64", {BT::i64}, BT::i64, "beans_bytes_get_i64", true, bytes_get_i64},
        {BT::bytes, "put_u8", {BT::i64, BT::i64}, BT::self_recv, "beans_bytes_put_u8", true, bytes_put_u8},
        {BT::bytes, "put_u16", {BT::i64, BT::i64}, BT::self_recv, "beans_bytes_put_u16", true, bytes_put_u16},
        {BT::bytes, "put_u32", {BT::i64, BT::i64}, BT::self_recv, "beans_bytes_put_u32", true, bytes_put_u32},
        {BT::bytes, "put_u64", {BT::i64, BT::i64}, BT::self_recv, "beans_bytes_put_u64", true, bytes_put_u64},
        {BT::bytes, "put_i64", {BT::i64, BT::i64}, BT::self_recv, "beans_bytes_put_i64", true, bytes_put_i64},
        {BT::bytes, "slice", {BT::i64, BT::i64}, BT::bytes, "beans_bytes_slice", true, bytes_slice},
        {BT::bytes, "copy_from", {BT::bytes, BT::i64}, BT::self_recv, "beans_bytes_copy_from", true, bytes_copy_from},
        {BT::bytes, "append", {BT::bytes}, BT::self_recv, "beans_bytes_append", false, bytes_append},
        {BT::bytes, "append_str", {BT::str}, BT::self_recv, "beans_bytes_append_str", false, bytes_append_str},
        {BT::bytes, "to_string", {}, BT::str, "beans_bytes_to_string", false, bytes_to_string},
    };
    return table;
}

const std::vector<BuiltinStatic>& builtin_statics() {
    static const std::vector<BuiltinStatic> table = {
        {"Bytes", "new", {BT::i64}, BT::bytes, "beans_bytes_new", true, bytes_new},
        {"Bytes", "from", {BT::str}, BT::bytes, "beans_bytes_from", false, bytes_from},
    };
    return table;
}

} // namespace beans
