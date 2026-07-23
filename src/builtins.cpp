#include "builtins.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "interp.h" // BeansPanic — builtin rows panic with the same messages
                    // the C runtime prints; the two must stay byte-identical

namespace beans {

FileVal::~FileVal() {
    // close whatever is still open: a live handle (never closed) or one whose
    // close() was deferred while worker threads ran (fd left valid). The last
    // ref is gone, so no thread can be mid-op — releasing now is safe.
    if (fd >= 0) close(fd);
}

MMapVal::~MMapVal() {
    if (ptr) munmap(ptr, static_cast<size_t>(len));
    if (fd >= 0) close(fd);
}

namespace {

std::vector<std::string> g_args;

[[noreturn]] void bpanic(uint32_t line, uint32_t col, std::string msg) {
    g_beans_panicking = true; // deinit stays silent while this unwinds
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
Value res_err(std::string msg, std::string kind = "") {
    Value e;
    e.k = Value::K::instance;
    e.inst = std::make_shared<InstanceVal>();
    e.inst->fields.emplace_back("msg", Value::of_str(std::move(msg)));
    e.inst->fields.emplace_back("kind", Value::of_str(std::move(kind)));
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Result";
    x.en->variant = "err";
    x.en->payload.push_back(std::move(e));
    return x;
}

// errno -> Error.kind slug; message is "path: strerror" — the C runtime
// builds the identical pair
const char* fs_kind_of(int err) {
    switch (err) {
        case ENOENT: return "not_found";
        case EACCES:
        case EPERM: return "permission";
        case EEXIST: return "exists";
        case ENOTDIR: return "not_dir";
        case EISDIR: return "is_dir";
        case ENOTEMPTY: return "not_empty";
        default: return "io";
    }
}

Value fs_err(const std::string& path, int err) {
    return res_err(path + ": " + strerror(err), fs_kind_of(err));
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

Value str_find_byte(uint32_t line, uint32_t col, Value& recv,
                    std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t byte = args[0].i, from = args[1].i;
    if (byte < 0 || byte > 255)
        bpanic(line, col, "byte " + std::to_string(byte) + " out of range");
    if (from < 0 || static_cast<size_t>(from) > s.size())
        bpanic(line, col, "find start " + std::to_string(from) +
                              " out of range (len " + std::to_string(s.size()) + ")");
    size_t found = s.find(static_cast<char>(byte), static_cast<size_t>(from));
    return Value::of_int(found == std::string::npos ? -1
                                                     : static_cast<int64_t>(found));
}

Value str_range_equals(uint32_t line, uint32_t col, Value& recv,
                       std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t from = args[0].i, to = args[1].i;
    if (from < 0 || to < from || static_cast<size_t>(to) > s.size())
        bpanic(line, col, "range " + std::to_string(from) + ".." +
                              std::to_string(to) + " out of range (len " +
                              std::to_string(s.size()) + ")");
    const std::string& other = *args[2].s;
    size_t length = static_cast<size_t>(to - from);
    return Value::of_bool(length == other.size() &&
                          memcmp(s.data() + from, other.data(), length) == 0);
}

Value str_parse_int_range_or(uint32_t line, uint32_t col, Value& recv,
                             std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t from = args[0].i, to = args[1].i, fallback = args[2].i;
    if (from < 0 || to < from || static_cast<size_t>(to) > s.size())
        bpanic(line, col, "range " + std::to_string(from) + ".." +
                              std::to_string(to) + " out of range (len " +
                              std::to_string(s.size()) + ")");
    size_t at = static_cast<size_t>(from), end = static_cast<size_t>(to);
    bool negative = false;
    if (at < end && (s[at] == '+' || s[at] == '-')) {
        negative = s[at] == '-';
        at += 1;
    }
    if (at == end) return Value::of_int(fallback);
    uint64_t value = 0;
    for (; at < end; ++at) {
        unsigned char c = static_cast<unsigned char>(s[at]);
        if (c < '0' || c > '9') return Value::of_int(fallback);
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (negative) value = 0 - value;
    return Value::of_int(static_cast<int64_t>(value));
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
            // exponent magnitude is capped at 4096: it must fit an int32 on
            // this side and a bounded pow10 walk on both — past the cap the
            // string simply does not read as a decimal
            long long ev = 0;
            while (s[i] >= '0' && s[i] <= '9') {
                ev = ev * 10 + (s[i] - '0');
                if (ev > 4096) return false;
                i++;
            }
            return s[i] == '\0';
        }
        return false;
    }
    return digits > 0;
}

// UTF-8 sequences, one string per character; a malformed lead or truncated
// tail comes through one byte at a time — byte slicing, no validation
Value str_chars(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    const std::string& s = *recv.s;
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80          ? 1
                   : (c >> 5) == 0x6 ? 2
                   : (c >> 4) == 0xE ? 3
                   : (c >> 3) == 0x1E ? 4
                                      : 1;
        if (i + n > s.size()) {
            n = 1;
        } else {
            for (size_t k = 1; k < n; k++) {
                if ((static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) {
                    n = 1;
                    break;
                }
            }
        }
        out.push_back(s.substr(i, n));
        i += n;
    }
    return str_list(std::move(out));
}

Value str_count_chars(uint32_t line, uint32_t col, Value& recv,
                      std::vector<Value>& args) {
    const std::string& s = *recv.s;
    int64_t from = args[0].i, to = args[1].i;
    if (from < 0 || to < from || to > static_cast<int64_t>(s.size())) {
        bpanic(line, col, "character range " + std::to_string(from) + ".." +
                              std::to_string(to) + " out of range (len " +
                              std::to_string(s.size()) + ")");
    }
    int64_t count = 0;
    size_t i = static_cast<size_t>(from), end = static_cast<size_t>(to);
    while (i < end) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80            ? 1
                   : (c >> 5) == 0x6   ? 2
                   : (c >> 4) == 0xE   ? 3
                   : (c >> 3) == 0x1E  ? 4
                                       : 1;
        if (i + n > end) {
            n = 1;
        } else {
            for (size_t k = 1; k < n; k++) {
                if ((static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) {
                    n = 1;
                    break;
                }
            }
        }
        i += n;
        count += 1;
    }
    return Value::of_int(count);
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

Value bytes_reserve(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    int64_t n = args[0].i;
    if (n < 0) bpanic(line, col, "negative reserve capacity " + std::to_string(n));
    if (n > (1LL << 58)) bpanic(line, col, "reserve capacity too large");
    recv.bytes->data.reserve(static_cast<size_t>(n));
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

Value bytes_push(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    recv.bytes->data.push_back(static_cast<uint8_t>(args[0].i & 255));
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

Value bytes_append_i64(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    uint64_t bits = static_cast<uint64_t>(args[0].i);
    auto& data = recv.bytes->data;
    for (unsigned shift = 0; shift < 64; shift += 8)
        data.push_back(static_cast<uint8_t>(bits >> shift));
    return recv;
}

Value bytes_append_range(uint32_t line, uint32_t col, Value& recv,
                         std::vector<Value>& args) {
    auto& source = args[0].bytes->data;
    int64_t from = args[1].i, to = args[2].i;
    if (from < 0 || to < from || to > static_cast<int64_t>(source.size())) {
        bpanic(line, col, "slice " + std::to_string(from) + ".." +
                              std::to_string(to) + " out of range (len " +
                              std::to_string(source.size()) + ")");
    }
    std::vector<uint8_t> copy;
    const uint8_t* first = source.data() + from;
    const uint8_t* last = source.data() + to;
    if (&source == &recv.bytes->data) {
        copy.assign(first, last);
        recv.bytes->data.insert(recv.bytes->data.end(), copy.begin(), copy.end());
    } else {
        recv.bytes->data.insert(recv.bytes->data.end(), first, last);
    }
    return recv;
}

Value bytes_to_string(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    auto& d = recv.bytes->data;
    size_t n = 0;
    while (n < d.size() && d[n] != 0) n++; // strings are text: stop at NUL
    return Value::of_str(std::string(reinterpret_cast<const char*>(d.data()), n));
}

Value bytes_to_string_full(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    auto& d = recv.bytes->data;
    return Value::of_str(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
}

// ---- File / Dir -------------------------------------------------------------

Value file_val(int fd) {
    Value v;
    v.k = Value::K::file;
    v.file = std::make_shared<FileVal>();
    v.file->fd = fd;
    return v;
}

Value bytes_val(std::vector<uint8_t> data) {
    Value v;
    v.k = Value::K::bytes;
    v.bytes = std::make_shared<BytesVal>();
    v.bytes->data = std::move(data);
    return v;
}

Value closed_err() { return res_err("file is closed", "closed"); }

Value file_read_at(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    if (recv.file->closed) return closed_err();
    int64_t pos = args[0].i, n = args[1].i;
    if (pos < 0 || n < 0) return res_err("negative read", "io");
    struct stat st;
    if (fstat(recv.file->fd, &st) == 0 && S_ISREG(st.st_mode)) {
        int64_t rem = st.st_size > pos ? st.st_size - pos : 0;
        if (n > rem) n = rem;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t r = pread(recv.file->fd, buf.data() + got, buf.size() - got,
                          static_cast<off_t>(pos) + static_cast<off_t>(got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return res_err(std::string("read: ") + strerror(errno), fs_kind_of(errno));
        }
        if (r == 0) break; // eof: a short read returns what's there
        got += static_cast<size_t>(r);
    }
    buf.resize(got);
    return res_ok(bytes_val(std::move(buf)));
}

Value file_write_at(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    if (recv.file->closed) return closed_err();
    int64_t pos = args[0].i;
    auto& d = args[1].bytes->data;
    if (pos < 0) return res_err("negative write", "io");
    size_t done = 0;
    while (done < d.size()) {
        ssize_t r = pwrite(recv.file->fd, d.data() + done, d.size() - done,
                           static_cast<off_t>(pos) + static_cast<off_t>(done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return res_err(std::string("write: ") + strerror(errno), fs_kind_of(errno));
        }
        done += static_cast<size_t>(r);
    }
    return res_ok(Value::of_int(static_cast<int64_t>(done)));
}

Value file_read(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    if (recv.file->closed) return closed_err();
    int64_t n = args[0].i;
    if (n < 0) return res_err("negative read", "io");
    // clamp to what the file can actually give: a corrupted length field must
    // not become a giant allocation — the read comes back short anyway
    struct stat st;
    if (fstat(recv.file->fd, &st) == 0 && S_ISREG(st.st_mode)) {
        int64_t at = static_cast<int64_t>(lseek(recv.file->fd, 0, SEEK_CUR));
        int64_t rem = at >= 0 && st.st_size > at ? st.st_size - at : 0;
        if (n > rem) n = rem;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t r = read(recv.file->fd, buf.data() + got, buf.size() - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return res_err(std::string("read: ") + strerror(errno), fs_kind_of(errno));
        }
        if (r == 0) break;
        got += static_cast<size_t>(r);
    }
    buf.resize(got);
    return res_ok(bytes_val(std::move(buf)));
}

Value file_write(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    if (recv.file->closed) return closed_err();
    auto& d = args[0].bytes->data;
    size_t done = 0;
    while (done < d.size()) {
        ssize_t r = write(recv.file->fd, d.data() + done, d.size() - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            return res_err(std::string("write: ") + strerror(errno), fs_kind_of(errno));
        }
        done += static_cast<size_t>(r);
    }
    return res_ok(Value::of_int(static_cast<int64_t>(done)));
}

Value file_seek(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    if (recv.file->closed) bpanic(line, col, "file is closed");
    off_t r = lseek(recv.file->fd, static_cast<off_t>(args[0].i), SEEK_SET);
    if (r < 0) {
        bpanic(line, col, "seek to " + std::to_string(args[0].i) + ": " + strerror(errno));
    }
    return Value::of_int(static_cast<int64_t>(r));
}

Value file_seek_end(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    if (recv.file->closed) bpanic(line, col, "file is closed");
    off_t r = lseek(recv.file->fd, static_cast<off_t>(args[0].i), SEEK_END);
    if (r < 0) {
        bpanic(line, col, "seek to " + std::to_string(args[0].i) + ": " + strerror(errno));
    }
    return Value::of_int(static_cast<int64_t>(r));
}

Value file_tell(uint32_t line, uint32_t col, Value& recv, std::vector<Value>&) {
    if (recv.file->closed) bpanic(line, col, "file is closed");
    return Value::of_int(static_cast<int64_t>(lseek(recv.file->fd, 0, SEEK_CUR)));
}

Value file_size_m(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    if (recv.file->closed) return closed_err();
    struct stat st;
    if (fstat(recv.file->fd, &st) != 0) {
        return res_err(std::string("size: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_int(static_cast<int64_t>(st.st_size)));
}

Value file_truncate(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    if (recv.file->closed) return closed_err();
    if (ftruncate(recv.file->fd, static_cast<off_t>(args[0].i)) != 0) {
        return res_err(std::string("truncate: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_bool(true));
}

Value file_sync(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    if (recv.file->closed) return closed_err();
    if (fsync(recv.file->fd) != 0) {
        return res_err(std::string("sync: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_bool(true));
}

Value file_close(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    if (recv.file->closed) return res_err("file already closed", "closed");
    recv.file->closed = true;
    // defer the real close() while workers run: a racing op on another thread
    // could still be mid-syscall on this fd, and reusing the number would
    // corrupt a freshly-opened file. The destructor closes it once the last
    // ref drops. Mirrors the native runtime's cc_threads gate; zero cost with
    // no threads.
    if (beans_threads_live()) return res_ok(Value::of_bool(true));
    int fd = recv.file->fd;
    recv.file->fd = -1;
    if (close(fd) != 0) {
        return res_err(std::string("close: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_bool(true));
}

// advisory flock — single-writer databases; try_lock's ok(false) means "held
// by someone else", every other failure is a real error
Value file_lock(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    if (recv.file->closed) return closed_err();
    // a blocking lock is the classic EINTR victim — retry rather than fail
    while (flock(recv.file->fd, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        return res_err(std::string("lock: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_bool(true));
}

Value file_try_lock(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    if (recv.file->closed) return closed_err();
    if (flock(recv.file->fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) return res_ok(Value::of_bool(false));
        return res_err(std::string("try_lock: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_bool(true));
}

Value file_unlock(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    if (recv.file->closed) return closed_err();
    if (flock(recv.file->fd, LOCK_UN) != 0) {
        return res_err(std::string("unlock: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_bool(true));
}

// ---- varint / crc32 ---------------------------------------------------------
// unsigned LEB128 over the 64-bit two's-complement pattern (negatives take
// 10 bytes); crc32 is the IEEE polynomial, table-driven — the C runtime
// computes the identical table

Value bytes_append_varint(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    uint64_t v = static_cast<uint64_t>(args[0].i);
    auto& d = recv.bytes->data;
    while (v >= 0x80) {
        d.push_back(static_cast<uint8_t>(v) | 0x80);
        v >>= 7;
    }
    d.push_back(static_cast<uint8_t>(v));
    return recv;
}

Value bytes_get_varint(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    auto& d = recv.bytes->data;
    int64_t pos = args[0].i;
    uint64_t v = 0;
    int shift = 0;
    size_t i = pos < 0 ? d.size() : static_cast<size_t>(pos);
    while (true) {
        if (pos < 0 || i >= d.size()) {
            bpanic(line, col, "varint read at " + std::to_string(pos) +
                                  " out of range (len " + std::to_string(d.size()) + ")");
        }
        if (shift >= 64) {
            bpanic(line, col, "varint too long at " + std::to_string(pos));
        }
        uint8_t b = d[i++];
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return Value::of_int(static_cast<int64_t>(v));
}

Value bytes_varint_size_s(uint32_t, uint32_t, std::vector<Value>& args) {
    uint64_t v = static_cast<uint64_t>(args[0].i);
    int64_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return Value::of_int(n);
}

uint32_t crc_table_cpp[256];
bool crc_ready_cpp = false;

Value bytes_crc32(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    auto& d = recv.bytes->data;
    int64_t from = args[0].i, to = args[1].i;
    if (from < 0 || to < from || to > static_cast<int64_t>(d.size())) {
        bpanic(line, col, "crc32 " + std::to_string(from) + ".." + std::to_string(to) +
                              " out of range (len " + std::to_string(d.size()) + ")");
    }
    if (!crc_ready_cpp) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table_cpp[i] = c;
        }
        crc_ready_cpp = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (int64_t i = from; i < to; i++) {
        c = crc_table_cpp[(c ^ d[static_cast<size_t>(i)]) & 0xFF] ^ (c >> 8);
    }
    return Value::of_int(static_cast<int64_t>(c ^ 0xFFFFFFFFu));
}

// ---- MMap -------------------------------------------------------------------
// whole-file shared mapping; the fd is kept open so resize() can remap — the
// mapping outlives it. get/put/read/write panic on a closed or short map,
// flush/close report errors as Results like File does.

Value mmap_closed_res() { return res_err("mmap is closed", "closed"); }

Value mmap_len(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    return Value::of_int(recv.mm->len);
}

Value mmap_get_w(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args,
                 const char* what, int64_t w) {
    MMapVal& m = *recv.mm;
    if (m.closed) bpanic(line, col, "mmap is closed");
    int64_t pos = args[0].i;
    // pos > len - w, never pos + w > len: the addition overflows for huge pos
    // and the wrapped sum sails past the guard into a wild read
    if (pos < 0 || w > m.len || pos > m.len - w) {
        bpanic(line, col, std::string(what) + " read at " + std::to_string(pos) +
                              " out of range (len " + std::to_string(m.len) + ")");
    }
    uint64_t v = 0;
    memcpy(&v, static_cast<char*>(m.ptr) + pos, static_cast<size_t>(w));
    return Value::of_int(static_cast<int64_t>(v));
}

Value mmap_put_w(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args,
                 const char* what, int64_t w) {
    MMapVal& m = *recv.mm;
    if (m.closed) bpanic(line, col, "mmap is closed");
    if (!m.writable) bpanic(line, col, "mmap is read-only");
    int64_t pos = args[0].i;
    if (pos < 0 || w > m.len || pos > m.len - w) {
        bpanic(line, col, std::string(what) + " write at " + std::to_string(pos) +
                              " out of range (len " + std::to_string(m.len) + ")");
    }
    uint64_t v = static_cast<uint64_t>(args[1].i);
    memcpy(static_cast<char*>(m.ptr) + pos, &v, static_cast<size_t>(w));
    return recv;
}

Value mmap_get_u8(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_get_w(l, c, r, a, "u8", 1); }
Value mmap_get_u16(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_get_w(l, c, r, a, "u16", 2); }
Value mmap_get_u32(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_get_w(l, c, r, a, "u32", 4); }
Value mmap_get_u64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_get_w(l, c, r, a, "u64", 8); }
Value mmap_get_i64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_get_w(l, c, r, a, "i64", 8); }
Value mmap_put_u8(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_put_w(l, c, r, a, "u8", 1); }
Value mmap_put_u16(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_put_w(l, c, r, a, "u16", 2); }
Value mmap_put_u32(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_put_w(l, c, r, a, "u32", 4); }
Value mmap_put_u64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_put_w(l, c, r, a, "u64", 8); }
Value mmap_put_i64(uint32_t l, uint32_t c, Value& r, std::vector<Value>& a) { return mmap_put_w(l, c, r, a, "i64", 8); }

Value mmap_read(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    MMapVal& m = *recv.mm;
    if (m.closed) bpanic(line, col, "mmap is closed");
    int64_t pos = args[0].i, n = args[1].i;
    if (pos < 0 || n < 0 || n > m.len || pos > m.len - n) {
        bpanic(line, col, "read " + std::to_string(n) + " at " + std::to_string(pos) +
                              " out of range (len " + std::to_string(m.len) + ")");
    }
    Value v;
    v.k = Value::K::bytes;
    v.bytes = std::make_shared<BytesVal>();
    const uint8_t* p = static_cast<const uint8_t*>(m.ptr) + pos;
    v.bytes->data.assign(p, p + n);
    return v;
}

Value mmap_write(uint32_t line, uint32_t col, Value& recv, std::vector<Value>& args) {
    MMapVal& m = *recv.mm;
    if (m.closed) bpanic(line, col, "mmap is closed");
    if (!m.writable) bpanic(line, col, "mmap is read-only");
    auto& d = args[1].bytes->data;
    int64_t pos = args[0].i;
    int64_t wn = static_cast<int64_t>(d.size());
    if (pos < 0 || wn > m.len || pos > m.len - wn) {
        bpanic(line, col, "write " + std::to_string(d.size()) + " at " +
                              std::to_string(pos) + " out of range (len " +
                              std::to_string(m.len) + ")");
    }
    memcpy(static_cast<char*>(m.ptr) + pos, d.data(), d.size());
    return recv;
}

Value mmap_flush(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    MMapVal& m = *recv.mm;
    if (m.closed) return mmap_closed_res();
    if (m.len > 0 && msync(m.ptr, static_cast<size_t>(m.len), MS_SYNC) != 0) {
        return res_err(std::string("flush: ") + strerror(errno), fs_kind_of(errno));
    }
    return res_ok(Value::of_bool(true));
}

Value mmap_flush_range(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    MMapVal& m = *recv.mm;
    if (m.closed) return mmap_closed_res();
    int64_t pos = args[0].i, n = args[1].i;
    if (pos < 0 || n < 0 || n > m.len || pos > m.len - n) {
        return res_err("flush " + std::to_string(n) + " at " + std::to_string(pos) +
                           " out of range (len " + std::to_string(m.len) + ")",
                       "io");
    }
    if (n > 0) {
        int64_t page = static_cast<int64_t>(getpagesize());
        int64_t start = pos - pos % page; // msync wants a page-aligned base
        if (msync(static_cast<char*>(m.ptr) + start,
                  static_cast<size_t>(pos + n - start), MS_SYNC) != 0) {
            return res_err(std::string("flush: ") + strerror(errno),
                           fs_kind_of(errno));
        }
    }
    return res_ok(Value::of_bool(true));
}

Value mmap_close(uint32_t, uint32_t, Value& recv, std::vector<Value>&) {
    MMapVal& m = *recv.mm;
    if (m.closed) return res_err("mmap already closed", "closed");
    m.closed = true;
    // defer munmap+close while workers run (see file_close): a racing op must
    // not have the mapping pulled out from under it
    if (beans_threads_live()) return res_ok(Value::of_bool(true));
    bool bad = m.ptr && munmap(m.ptr, static_cast<size_t>(m.len)) != 0;
    int e = errno;
    m.ptr = nullptr;
    if (m.fd >= 0) close(m.fd);
    m.fd = -1;
    if (bad) return res_err(std::string("close: ") + strerror(e), fs_kind_of(e));
    return res_ok(Value::of_bool(true));
}

// grow or shrink in place: truncate the file, drop the old mapping, map
// fresh. On a mapping failure the handle stays open but empty (len 0).
Value mmap_resize(uint32_t, uint32_t, Value& recv, std::vector<Value>& args) {
    MMapVal& m = *recv.mm;
    if (m.closed) return mmap_closed_res();
    if (!m.writable) return res_err("mmap is read-only", "permission");
    int64_t n = args[0].i;
    if (n < 0) return res_err("negative resize", "io");
    if (ftruncate(m.fd, static_cast<off_t>(n)) != 0) {
        return res_err(std::string("resize: ") + strerror(errno), fs_kind_of(errno));
    }
    if (m.ptr) munmap(m.ptr, static_cast<size_t>(m.len));
    m.ptr = nullptr;
    m.len = 0;
    if (n > 0) {
        void* p = mmap(nullptr, static_cast<size_t>(n), PROT_READ | PROT_WRITE,
                       MAP_SHARED, m.fd, 0);
        if (p == MAP_FAILED) {
            return res_err(std::string("resize: ") + strerror(errno),
                           fs_kind_of(errno));
        }
        m.ptr = p;
        m.len = n;
    }
    return res_ok(Value::of_bool(true));
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

Value file_exists_s(uint32_t, uint32_t, std::vector<Value>& args) {
    struct stat st;
    return Value::of_bool(stat(args[0].s->c_str(), &st) == 0 && !S_ISDIR(st.st_mode));
}

Value file_size_s(uint32_t, uint32_t, std::vector<Value>& args) {
    struct stat st;
    if (stat(args[0].s->c_str(), &st) != 0) return fs_err(*args[0].s, errno);
    return res_ok(Value::of_int(static_cast<int64_t>(st.st_size)));
}

Value file_remove_s(uint32_t, uint32_t, std::vector<Value>& args) {
    const std::string& p = *args[0].s;
    struct stat st;
    // lstat, not stat: a dangling symlink has no target to stat, and even a
    // live one should be unlinked as the link, not followed to its target
    if (lstat(p.c_str(), &st) != 0) return fs_err(p, errno);
    int r = S_ISDIR(st.st_mode) ? rmdir(p.c_str()) : unlink(p.c_str());
    if (r != 0) return fs_err(p, errno);
    return res_ok(Value::of_bool(true));
}

Value file_rename_s(uint32_t, uint32_t, std::vector<Value>& args) {
    if (rename(args[0].s->c_str(), args[1].s->c_str()) != 0) {
        return fs_err(*args[0].s, errno);
    }
    return res_ok(Value::of_bool(true));
}

Value file_open_s(uint32_t, uint32_t, std::vector<Value>& args) {
    const std::string& path = *args[0].s;
    const std::string& mode = *args[1].s;
    int flags;
    if (mode == "r") flags = O_RDONLY;
    else if (mode == "rw") flags = O_RDWR;
    else if (mode == "create") flags = O_RDWR | O_CREAT;
    else if (mode == "append") flags = O_WRONLY | O_CREAT | O_APPEND;
    else return res_err("bad open mode '" + mode + "'", "io");
    int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) return fs_err(path, errno);
    return res_ok(file_val(fd));
}

Value dir_make_s(uint32_t, uint32_t, std::vector<Value>& args) {
    if (mkdir(args[0].s->c_str(), 0755) != 0) return fs_err(*args[0].s, errno);
    return res_ok(Value::of_bool(true));
}

Value dir_make_all_s(uint32_t, uint32_t, std::vector<Value>& args) {
    const std::string& p = *args[0].s;
    std::string cur;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        cur = p.substr(0, j);
        i = j + 1;
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), 0755) != 0) {
            if (errno != EEXIST) return fs_err(cur, errno);
            // EEXIST is only ok if it's already a directory — a regular file
            // in the path is a real error (Go's MkdirAll semantics)
            struct stat st;
            if (stat(cur.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                return fs_err(cur, ENOTDIR);
        }
    }
    return res_ok(Value::of_bool(true));
}

Value dir_list_s(uint32_t, uint32_t, std::vector<Value>& args) {
    const std::string& p = *args[0].s;
    DIR* d = opendir(p.c_str());
    if (!d) return fs_err(p, errno);
    std::vector<std::string> names;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        names.push_back(de->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end()); // deterministic for diff tests
    return res_ok(str_list(std::move(names)));
}

Value dir_remove_s(uint32_t, uint32_t, std::vector<Value>& args) {
    if (rmdir(args[0].s->c_str()) != 0) return fs_err(*args[0].s, errno);
    return res_ok(Value::of_bool(true));
}

// Iterative delete: gather the tree pre-order (parent before child) one DIR
// at a time, then rmdir in reverse (deepest first). Recursion held one fd per
// level and ran out on deep trees; this caps open fds at one.
int rm_tree(const std::string& p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p.c_str());
    std::vector<std::string> dirs;  // to rmdir, discovery (pre) order
    std::vector<std::string> stack{p};
    while (!stack.empty()) {
        std::string dir = std::move(stack.back());
        stack.pop_back();
        DIR* d = opendir(dir.c_str());
        if (!d) return -1;
        while (dirent* de = readdir(d)) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            std::string sub = dir + "/" + de->d_name;
            struct stat cst;
            if (lstat(sub.c_str(), &cst) != 0) {
                closedir(d);
                return -1;
            }
            if (S_ISDIR(cst.st_mode)) stack.push_back(sub); // scan later
            else if (unlink(sub.c_str()) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        dirs.push_back(std::move(dir)); // remove after its children
    }
    for (size_t i = dirs.size(); i-- > 0;) { // deepest first
        if (rmdir(dirs[i].c_str()) != 0) return -1;
    }
    return 0;
}

Value dir_remove_all_s(uint32_t, uint32_t, std::vector<Value>& args) {
    struct stat st;
    if (lstat(args[0].s->c_str(), &st) != 0) return fs_err(*args[0].s, errno);
    if (rm_tree(*args[0].s) != 0) return fs_err(*args[0].s, errno);
    return res_ok(Value::of_bool(true));
}

Value dir_exists_s(uint32_t, uint32_t, std::vector<Value>& args) {
    struct stat st;
    return Value::of_bool(stat(args[0].s->c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

std::string temp_dir_path() {
    const char* t = getenv("TMPDIR");
    std::string p = t && *t ? t : "/tmp";
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

Value dir_temp_s(uint32_t, uint32_t, std::vector<Value>&) {
    return Value::of_str(temp_dir_path());
}

Value dir_sync_s(uint32_t, uint32_t, std::vector<Value>& args) {
    // the database commit pattern: fsync the directory after a rename
    int fd = open(args[0].s->c_str(), O_RDONLY);
    if (fd < 0) return fs_err(*args[0].s, errno);
    if (fsync(fd) != 0) {
        int e = errno;
        close(fd);
        return fs_err(*args[0].s, e);
    }
    close(fd);
    return res_ok(Value::of_bool(true));
}

// files and symlinks under root (lstat — never follows a link), paths
// relative to root, "/"-joined, sorted at the end so readdir order is moot
// Iterative walk: an explicit stack of relative dir paths, one DIR open at a
// time. Recursion held one fd per depth level and ran out at ~250 deep; this
// caps open fds at one. Output is sorted by the caller, so order is free.
bool walk_dir(const std::string& root, const std::string&,
              std::vector<std::string>& out, std::string& epath, int& eno) {
    std::vector<std::string> stack{""}; // "" = root itself
    while (!stack.empty()) {
        std::string rel = std::move(stack.back());
        stack.pop_back();
        std::string full = rel.empty() ? root : root + "/" + rel;
        DIR* d = opendir(full.c_str());
        if (!d) {
            epath = full;
            eno = errno;
            return false;
        }
        while (dirent* en = readdir(d)) {
            std::string name = en->d_name;
            if (name == "." || name == "..") continue;
            std::string r2 = rel.empty() ? name : rel + "/" + name;
            struct stat st;
            if (lstat((root + "/" + r2).c_str(), &st) != 0) {
                epath = root + "/" + r2;
                eno = errno;
                closedir(d);
                return false;
            }
            if (S_ISDIR(st.st_mode)) stack.push_back(r2); // descend later
            else out.push_back(r2);
        }
        closedir(d);
    }
    return true;
}

Value dir_walk_s(uint32_t, uint32_t, std::vector<Value>& args) {
    std::vector<std::string> out;
    std::string epath;
    int eno = 0;
    if (!walk_dir(*args[0].s, "", out, epath, eno)) return fs_err(epath, eno);
    std::sort(out.begin(), out.end());
    return res_ok(str_list(std::move(out)));
}

Value mmap_open_s(uint32_t, uint32_t, std::vector<Value>& args) {
    const std::string& path = *args[0].s;
    bool writable = args[1].b;
    int fd = open(path.c_str(), writable ? O_RDWR : O_RDONLY);
    if (fd < 0) return fs_err(path, errno);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return fs_err(path, e);
    }
    auto mv = std::make_shared<MMapVal>();
    mv->len = static_cast<int64_t>(st.st_size);
    mv->writable = writable;
    if (mv->len > 0) {
        void* p = mmap(nullptr, static_cast<size_t>(mv->len),
                       writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED,
                       fd, 0);
        if (p == MAP_FAILED) {
            int e = errno;
            close(fd);
            return fs_err(path, e);
        }
        mv->ptr = p;
    }
    mv->fd = fd; // kept: resize() needs it to ftruncate + remap
    Value v;
    v.k = Value::K::mmap;
    v.mm = std::move(mv);
    return res_ok(std::move(v));
}

// ---- std.os / std.io --------------------------------------------------------

Value os_args(uint32_t, uint32_t, std::vector<Value>&) {
    return str_list(std::vector<std::string>(g_args));
}

Value os_env(uint32_t, uint32_t, std::vector<Value>& args) {
    const char* v = getenv(args[0].s->c_str());
    if (!v) return opt_none();
    return opt_some(Value::of_str(v));
}

Value os_exit(uint32_t, uint32_t, std::vector<Value>& args) {
    std::exit(static_cast<int>(args[0].i));
}

Value os_now_ms(uint32_t, uint32_t, std::vector<Value>&) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return Value::of_int(static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
}

Value os_ticks_ms(uint32_t, uint32_t, std::vector<Value>&) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return Value::of_int(static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
}

Value os_sleep_ms(uint32_t, uint32_t, std::vector<Value>& args) {
    int64_t ms = args[0].i;
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000;
        nanosleep(&ts, nullptr);
    }
    return Value::unit();
}

Value io_read_line(uint32_t, uint32_t, std::vector<Value>&) {
    std::string out;
    int c;
    bool any = false;
    while ((c = fgetc(stdin)) != EOF) {
        any = true;
        if (c == '\n') break;
        out.push_back(static_cast<char>(c));
    }
    if (!any) return opt_none();
    return opt_some(Value::of_str(std::move(out)));
}

Value io_read_all(uint32_t, uint32_t, std::vector<Value>&) {
    std::string out;
    char chunk[65536];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, stdin)) > 0) out.append(chunk, r);
    return Value::of_str(std::move(out));
}

// ---- std.fmt ----------------------------------------------------------------
// pure text rendering; the C runtime mirrors each of these byte for byte

Value fmt_pad_left(uint32_t line, uint32_t col, std::vector<Value>& args) {
    if (args[1].i > 1000000) bpanic(line, col, "pad width too large");
    return Value::of_str(fmt_pad_text(*args[0].s, args[1].i, false));
}

Value fmt_pad_right(uint32_t line, uint32_t col, std::vector<Value>& args) {
    if (args[1].i > 1000000) bpanic(line, col, "pad width too large");
    return Value::of_str(fmt_pad_text(*args[0].s, args[1].i, true));
}

Value fmt_float(uint32_t, uint32_t, std::vector<Value>& args) {
    return Value::of_str(fmt_float_text(args[0].f, args[1].i));
}

Value fmt_dec(uint32_t, uint32_t, std::vector<Value>& args) {
    return Value::of_str(fmt_dec_text(args[0].dec, args[1].i));
}

} // namespace

std::string fmt_pad_text(std::string s, int64_t width, bool left) {
    if (width <= static_cast<int64_t>(s.size())) return s;
    std::string pad(static_cast<size_t>(width) - s.size(), ' ');
    return left ? s + pad : pad + s;
}

std::string fmt_float_text(double x, int64_t places) {
    if (places < 0) places = 0;
    if (places > 100) places = 100;
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", static_cast<int>(places), x);
    return buf;
}

// exact places: round (half away from zero) when narrowing, zero-pad the
// rendered string when widening — no coefficient blow-up on large `places`
std::string fmt_dec_text(Decimal d, int64_t places) {
    if (places < 0) places = 0;
    if (places > 60) places = 60;
    if (static_cast<int32_t>(places) < d.scale) {
        d = d.round_to(static_cast<int32_t>(places));
    }
    std::string s = d.to_string();
    int64_t frac = d.scale;
    if (places > frac) {
        if (frac == 0) s += '.';
        s.append(static_cast<size_t>(places - frac), '0');
    }
    return s;
}

void set_program_args(std::vector<std::string> args) { g_args = std::move(args); }

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
        {BT::str, "find_byte", {BT::i64, BT::i64}, BT::i64, "beans_str_find_byte", true, str_find_byte},
        {BT::str, "range_equals", {BT::i64, BT::i64, BT::str}, BT::boolean, "beans_str_range_equals", true, str_range_equals},
        {BT::str, "parse_int_range_or", {BT::i64, BT::i64, BT::i64}, BT::i64, "beans_str_parse_int_range_or", true, str_parse_int_range_or},
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
        {BT::str, "chars", {}, BT::list_str, "beans_str_chars", false, str_chars},
        {BT::str, "count_chars", {BT::i64, BT::i64}, BT::i64, "beans_str_count_chars", true, str_count_chars},
        // Bytes
        {BT::bytes, "len", {}, BT::i64, "beans_bytes_len", false, bytes_len},
        {BT::bytes, "reserve", {BT::i64}, BT::self_recv, "beans_bytes_reserve", true, bytes_reserve},
        {BT::bytes, "resize", {BT::i64}, BT::self_recv, "beans_bytes_resize", true, bytes_resize},
        {BT::bytes, "fill", {BT::i64}, BT::self_recv, "beans_bytes_fill", false, bytes_fill},
        {BT::bytes, "get", {BT::i64}, BT::i64, "beans_bytes_get", true, bytes_get},
        {BT::bytes, "set", {BT::i64, BT::i64}, BT::self_recv, "beans_bytes_set", true, bytes_set},
        {BT::bytes, "push", {BT::i64}, BT::self_recv, "beans_bytes_push", false, bytes_push},
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
        {BT::bytes, "append_i64", {BT::i64}, BT::self_recv, "beans_bytes_append_i64", false, bytes_append_i64},
        {BT::bytes, "append_range", {BT::bytes, BT::i64, BT::i64}, BT::self_recv, "beans_bytes_append_range", true, bytes_append_range},
        {BT::bytes, "to_string", {}, BT::str, "beans_bytes_to_string", false, bytes_to_string},
        {BT::bytes, "to_string_full", {}, BT::str, "beans_bytes_to_string_full", false, bytes_to_string_full},
        {BT::bytes, "append_varint", {BT::i64}, BT::self_recv, "beans_bytes_append_varint", false, bytes_append_varint},
        {BT::bytes, "get_varint", {BT::i64}, BT::i64, "beans_bytes_get_varint", true, bytes_get_varint},
        {BT::bytes, "crc32", {BT::i64, BT::i64}, BT::i64, "beans_bytes_crc32", true, bytes_crc32},
        // File — positional I/O first: that's what a database wants
        {BT::file, "read_at", {BT::i64, BT::i64}, BT::res_bytes, "beans_file_read_at", false, file_read_at},
        {BT::file, "write_at", {BT::i64, BT::bytes}, BT::res_i64, "beans_file_write_at", false, file_write_at},
        {BT::file, "read", {BT::i64}, BT::res_bytes, "beans_file_read", false, file_read},
        {BT::file, "write", {BT::bytes}, BT::res_i64, "beans_file_write", false, file_write},
        {BT::file, "seek", {BT::i64}, BT::i64, "beans_file_seek", true, file_seek},
        {BT::file, "seek_end", {BT::i64}, BT::i64, "beans_file_seek_end", true, file_seek_end},
        {BT::file, "tell", {}, BT::i64, "beans_file_tell", true, file_tell},
        {BT::file, "size", {}, BT::res_i64, "beans_file_size", false, file_size_m},
        {BT::file, "truncate", {BT::i64}, BT::res_bool, "beans_file_truncate", false, file_truncate},
        {BT::file, "sync", {}, BT::res_bool, "beans_file_sync", false, file_sync},
        {BT::file, "close", {}, BT::res_bool, "beans_file_close", false, file_close},
        {BT::file, "lock", {}, BT::res_bool, "beans_file_lock", false, file_lock},
        {BT::file, "try_lock", {}, BT::res_bool, "beans_file_try_lock", false, file_try_lock},
        {BT::file, "unlock", {}, BT::res_bool, "beans_file_unlock", false, file_unlock},
        // MMap — bounds-checked words over a shared whole-file mapping
        {BT::mmap, "len", {}, BT::i64, "beans_mmap_len", false, mmap_len},
        {BT::mmap, "get_u8", {BT::i64}, BT::i64, "beans_mmap_get_u8", true, mmap_get_u8},
        {BT::mmap, "get_u16", {BT::i64}, BT::i64, "beans_mmap_get_u16", true, mmap_get_u16},
        {BT::mmap, "get_u32", {BT::i64}, BT::i64, "beans_mmap_get_u32", true, mmap_get_u32},
        {BT::mmap, "get_u64", {BT::i64}, BT::i64, "beans_mmap_get_u64", true, mmap_get_u64},
        {BT::mmap, "get_i64", {BT::i64}, BT::i64, "beans_mmap_get_i64", true, mmap_get_i64},
        {BT::mmap, "put_u8", {BT::i64, BT::i64}, BT::self_recv, "beans_mmap_put_u8", true, mmap_put_u8},
        {BT::mmap, "put_u16", {BT::i64, BT::i64}, BT::self_recv, "beans_mmap_put_u16", true, mmap_put_u16},
        {BT::mmap, "put_u32", {BT::i64, BT::i64}, BT::self_recv, "beans_mmap_put_u32", true, mmap_put_u32},
        {BT::mmap, "put_u64", {BT::i64, BT::i64}, BT::self_recv, "beans_mmap_put_u64", true, mmap_put_u64},
        {BT::mmap, "put_i64", {BT::i64, BT::i64}, BT::self_recv, "beans_mmap_put_i64", true, mmap_put_i64},
        {BT::mmap, "read", {BT::i64, BT::i64}, BT::bytes, "beans_mmap_read", true, mmap_read},
        {BT::mmap, "write", {BT::i64, BT::bytes}, BT::self_recv, "beans_mmap_write", true, mmap_write},
        {BT::mmap, "flush", {}, BT::res_bool, "beans_mmap_flush", false, mmap_flush},
        {BT::mmap, "flush_range", {BT::i64, BT::i64}, BT::res_bool, "beans_mmap_flush_range", false, mmap_flush_range},
        {BT::mmap, "resize", {BT::i64}, BT::res_bool, "beans_mmap_resize", false, mmap_resize},
        {BT::mmap, "close", {}, BT::res_bool, "beans_mmap_close", false, mmap_close},
    };
    return table;
}

const std::vector<BuiltinStatic>& builtin_statics() {
    static const std::vector<BuiltinStatic> table = {
        {"Bytes", "from", {BT::str}, BT::bytes, "beans_bytes_from", false, bytes_from},
        {"Bytes", "varint_size", {BT::i64}, BT::i64, "beans_bytes_varint_size", false, bytes_varint_size_s},
        {"File", "exists", {BT::str}, BT::boolean, "beans_file_exists", false, file_exists_s},
        {"File", "size", {BT::str}, BT::res_i64, "beans_file_size_p", false, file_size_s},
        {"File", "remove", {BT::str}, BT::res_bool, "beans_file_remove", false, file_remove_s},
        {"File", "rename", {BT::str, BT::str}, BT::res_bool, "beans_file_rename", false, file_rename_s},
        {"File", "open", {BT::str, BT::str}, BT::res_file, "beans_file_open", false, file_open_s},
        {"Dir", "make", {BT::str}, BT::res_bool, "beans_dir_make", false, dir_make_s},
        {"Dir", "make_all", {BT::str}, BT::res_bool, "beans_dir_make_all", false, dir_make_all_s},
        {"Dir", "list", {BT::str}, BT::res_list_str, "beans_dir_list", false, dir_list_s},
        {"Dir", "remove", {BT::str}, BT::res_bool, "beans_dir_remove", false, dir_remove_s},
        {"Dir", "remove_all", {BT::str}, BT::res_bool, "beans_dir_remove_all", false, dir_remove_all_s},
        {"Dir", "exists", {BT::str}, BT::boolean, "beans_dir_exists", false, dir_exists_s},
        {"Dir", "temp", {}, BT::str, "beans_dir_temp", false, dir_temp_s},
        {"Dir", "sync", {BT::str}, BT::res_bool, "beans_dir_sync", false, dir_sync_s},
        {"Dir", "walk", {BT::str}, BT::res_list_str, "beans_dir_walk", false, dir_walk_s},
        {"MMap", "open", {BT::str, BT::boolean}, BT::res_mmap, "beans_mmap_open", false, mmap_open_s},
    };
    return table;
}

const std::vector<BuiltinConstructor>& builtin_constructors() {
    static const std::vector<BuiltinConstructor> table = {
        {"Bytes", {BT::i64}, BT::bytes, "beans_bytes_new", true, bytes_new},
    };
    return table;
}

const std::vector<BuiltinFn>& builtin_fns() {
    static const std::vector<BuiltinFn> table = {
        {"std.os", "args", {}, BT::list_str, "beans_os_args", false, os_args},
        {"std.os", "env", {BT::str}, BT::opt_str, "beans_os_env", false, os_env},
        {"std.os", "exit", {BT::i64}, BT::unit, "beans_os_exit", false, os_exit},
        {"std.os", "now_ms", {}, BT::i64, "beans_os_now_ms", false, os_now_ms},
        {"std.os", "ticks_ms", {}, BT::i64, "beans_os_ticks_ms", false, os_ticks_ms},
        {"std.os", "sleep_ms", {BT::i64}, BT::unit, "beans_os_sleep_ms", false, os_sleep_ms},
        {"std.io", "read_line", {}, BT::opt_str, "beans_io_read_line", false, io_read_line},
        {"std.io", "read_all", {}, BT::str, "beans_io_read_all", false, io_read_all},
        {"std.fmt", "pad_left", {BT::str, BT::i64}, BT::str, "beans_fmt_pad_left", true, fmt_pad_left},
        {"std.fmt", "pad_right", {BT::str, BT::i64}, BT::str, "beans_fmt_pad_right", true, fmt_pad_right},
        {"std.fmt", "float", {BT::f64, BT::i64}, BT::str, "beans_fmt_float", false, fmt_float},
        {"std.fmt", "dec", {BT::dec, BT::i64}, BT::str, "beans_decv_fmt", false, fmt_dec},
    };
    return table;
}

} // namespace beans
