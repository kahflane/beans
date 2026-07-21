#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ast.h"

namespace beans {

// ---- exact base-10 decimal --------------------------------------------------
// value = coeff / 10^scale. 128-bit coefficient, C#-style.
struct Decimal {
    __int128 coeff = 0;
    int32_t scale = 0;

    static __int128 pow10(int32_t n) {
        __int128 p = 1;
        for (int32_t i = 0; i < n; i++) p *= 10;
        return p;
    }

    static Decimal from_int(int64_t v) { return {static_cast<__int128>(v), 0}; }

    // parse "19.99", "1_000.5", "250.00", "1.5e-9" — exact, from source text
    static Decimal parse(const std::string& text) {
        Decimal d;
        bool neg = false;
        int32_t exp = 0;
        size_t i = 0;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
            neg = text[i] == '-';
            i++;
        }
        bool after_dot = false;
        for (; i < text.size(); i++) {
            char c = text[i];
            if (c == '_') continue;
            if (c == '.') { after_dot = true; continue; }
            if (c == 'e' || c == 'E') {
                exp = static_cast<int32_t>(std::strtol(text.c_str() + i + 1, nullptr, 10));
                break;
            }
            d.coeff = d.coeff * 10 + (c - '0');
            if (after_dot) d.scale += 1;
        }
        d.scale -= exp;
        if (d.scale < 0) { // 1.5e9 → widen the coefficient
            d.coeff *= pow10(-d.scale);
            d.scale = 0;
        }
        if (neg) d.coeff = -d.coeff;
        return d;
    }

    // align two decimals to a common scale
    static void align(Decimal a, Decimal b, __int128& ca, __int128& cb, int32_t& s) {
        s = a.scale > b.scale ? a.scale : b.scale;
        ca = a.coeff * pow10(s - a.scale);
        cb = b.coeff * pow10(s - b.scale);
    }

    Decimal add(Decimal o) const {
        __int128 ca, cb; int32_t s;
        align(*this, o, ca, cb, s);
        return {ca + cb, s};
    }
    Decimal sub(Decimal o) const {
        __int128 ca, cb; int32_t s;
        align(*this, o, ca, cb, s);
        return {ca - cb, s};
    }
    Decimal mul(Decimal o) const { return {coeff * o.coeff, scale + o.scale}; }

    // division carries 20 extra digits, then trims trailing zeros
    Decimal div(Decimal o) const {
        int32_t extra = 20;
        __int128 num = coeff * pow10(extra + o.scale - (scale > o.scale ? 0 : 0));
        Decimal r{num / o.coeff, scale + extra};
        while (r.scale > 0 && r.coeff % 10 == 0) { r.coeff /= 10; r.scale -= 1; }
        return r;
    }

    Decimal neg() const { return {-coeff, scale}; }
    Decimal abs() const { return {coeff < 0 ? -coeff : coeff, scale}; }

    Decimal round_to(int32_t places) const {
        if (places >= scale) return *this;
        __int128 f = pow10(scale - places);
        __int128 q = coeff / f, rem = coeff % f;
        if (rem < 0) rem = -rem;
        // round half away from zero (banker's rounding is still an open question)
        if (rem * 2 >= f) q += coeff >= 0 ? 1 : -1;
        return {q, places};
    }

    int cmp(Decimal o) const {
        __int128 ca, cb; int32_t s;
        align(*this, o, ca, cb, s);
        return ca < cb ? -1 : ca > cb ? 1 : 0;
    }

    int64_t to_int() const { return static_cast<int64_t>(coeff / pow10(scale)); }
    double to_double() const {
        return static_cast<double>(coeff) / static_cast<double>(pow10(scale));
    }

    std::string to_string() const {
        __int128 c = coeff;
        bool neg = c < 0;
        if (neg) c = -c;
        std::string digits;
        if (c == 0) digits = "0";
        while (c > 0) {
            digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(c % 10)));
            c /= 10;
        }
        while (static_cast<int32_t>(digits.size()) <= scale) digits.insert(digits.begin(), '0');
        std::string out;
        if (neg) out += '-';
        out += digits.substr(0, digits.size() - scale);
        if (scale > 0) {
            out += '.';
            out += digits.substr(digits.size() - scale);
        }
        return out;
    }
};

// ---- runtime values ---------------------------------------------------------
// Value holds only pointers to its payload structs, so the payloads can be
// defined below (they contain Values by value).

struct ListVal;
struct MapVal;
struct InstanceVal;
struct EnumVal;
struct ClosureVal;
struct FnRefVal;
struct RangeVal;
struct ThreadVal;
struct MutexVal;
struct ChannelVal;
struct AtomicVal;

struct Value {
    enum class K {
        unit, int_, float_, decimal_, bool_, string_,
        list, map, instance, enum_v, closure, fn_ref, range,
        thread, mutex, channel, atomic,
    };
    K k = K::unit;

    int64_t i = 0;
    double f = 0;
    bool b = false;
    Decimal dec;
    std::shared_ptr<std::string> s;
    std::shared_ptr<ListVal> list;
    std::shared_ptr<MapVal> map;
    std::shared_ptr<InstanceVal> inst;
    std::shared_ptr<EnumVal> en;
    std::shared_ptr<ClosureVal> clo;
    std::shared_ptr<FnRefVal> fnr;
    std::shared_ptr<RangeVal> range;
    std::shared_ptr<ThreadVal> thread;
    std::shared_ptr<MutexVal> mutex;
    std::shared_ptr<ChannelVal> chan;
    std::shared_ptr<AtomicVal> atomic;

    static Value unit() { return {}; }
    static Value of_int(int64_t v) { Value x; x.k = K::int_; x.i = v; return x; }
    static Value of_float(double v) { Value x; x.k = K::float_; x.f = v; return x; }
    static Value of_dec(Decimal d) { Value x; x.k = K::decimal_; x.dec = d; return x; }
    static Value of_bool(bool v) { Value x; x.k = K::bool_; x.b = v; return x; }
    static Value of_str(std::string v) {
        Value x;
        x.k = K::string_;
        x.s = std::make_shared<std::string>(std::move(v));
        return x;
    }
};

struct ListVal { std::vector<Value> items; };
struct MapVal { std::vector<std::pair<Value, Value>> entries; };

struct InstanceVal {
    const ClassDecl* cls = nullptr;
    std::vector<std::pair<std::string, Value>> fields;

    Value* field(const std::string& name) {
        for (auto& [n, v] : fields) {
            if (n == name) return &v;
        }
        return nullptr;
    }
};

struct EnumVal {
    std::string enum_name;   // "Option", "Result", or a user enum
    std::string variant;
    std::vector<Value> payload;
};

struct Env;

struct ClosureVal {
    const Expr* node = nullptr;        // Expr::Kind::closure
    std::shared_ptr<Env> captured;
    std::string pkg;                   // package prefix where the closure was made
};

struct FnRefVal { const FnDecl* decl = nullptr; };

struct RangeVal { int64_t lo = 0, hi = 0; bool inclusive = false; };

struct ThreadVal {
    std::thread th;
    std::shared_ptr<Value> result;         // set by the worker
    std::shared_ptr<std::string> panic;    // set if the worker panicked
    bool joined = false;
    ~ThreadVal() {
        if (th.joinable()) th.detach();
    }
};

struct MutexVal {
    std::mutex m;
    std::shared_ptr<Value> inner;
};

struct ChannelVal {
    std::mutex m;
    std::condition_variable cv_send, cv_recv;
    std::deque<Value> q;
    size_t cap = 0;
    bool closed = false;
};

struct AtomicVal { std::atomic<int64_t> v{0}; };

// lexical scope chain — closures keep their captured chain alive
struct Env {
    std::shared_ptr<Env> parent;
    std::vector<std::pair<std::string, Value>> vars;

    Value* find(const std::string& name) {
        for (Env* e = this; e; e = e->parent.get()) {
            for (auto it = e->vars.rbegin(); it != e->vars.rend(); ++it) {
                if (it->first == name) return &it->second;
            }
        }
        return nullptr;
    }
    void declare(const std::string& name, Value v) {
        vars.emplace_back(name, std::move(v));
    }
};

} // namespace beans
