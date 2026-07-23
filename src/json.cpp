#include "json.h"

#include <cmath>
#include <cstdio>

namespace beans {

namespace {

// ---- serialize -----------------------------------------------------------

void escape_to(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c); // raw UTF-8 is valid JSON
                }
        }
    }
    out += '"';
}

void dump_to(std::string& out, const Json& j) {
    switch (j.t) {
        case Json::T::null: out += "null"; break;
        case Json::T::boolean: out += j.b ? "true" : "false"; break;
        case Json::T::number: {
            double v = j.num;
            if (std::floor(v) == v && std::abs(v) < 1e15) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
                out += buf;
            } else {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.17g", v);
                out += buf;
            }
            break;
        }
        case Json::T::string: escape_to(out, j.str); break;
        case Json::T::array: {
            out += '[';
            for (size_t i = 0; i < j.arr.size(); i++) {
                if (i) out += ',';
                dump_to(out, j.arr[i]);
            }
            out += ']';
            break;
        }
        case Json::T::object: {
            out += '{';
            for (size_t i = 0; i < j.obj.size(); i++) {
                if (i) out += ',';
                escape_to(out, j.obj[i].first);
                out += ':';
                dump_to(out, j.obj[i].second);
            }
            out += '}';
            break;
        }
    }
}

// ---- parse ---------------------------------------------------------------

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;

    explicit Parser(const std::string& src) : s(src) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                                s[i] == '\r'))
            i++;
    }
    char peek() { return i < s.size() ? s[i] : '\0'; }

    void put_utf8(std::string& out, unsigned cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4 && i < s.size(); k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else { ok = false; return 0; }
        }
        return v;
    }

    std::string parse_string() {
        std::string out;
        i++; // opening quote
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned cp = hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
                            s[i] == '\\' && s[i + 1] == 'u') {
                            i += 2;
                            unsigned lo = hex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        put_utf8(out, cp);
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        ok = false;
        return out;
    }

    Json parse_value() {
        ws();
        if (!ok || i >= s.size()) { ok = false; return Json::null(); }
        char c = s[i];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Json::string(parse_string());
        if (c == 't') { if (s.compare(i, 4, "true") == 0) { i += 4; return Json::boolean(true); } ok = false; return Json::null(); }
        if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; return Json::boolean(false); } ok = false; return Json::null(); }
        if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; return Json::null(); } ok = false; return Json::null(); }
        return parse_number();
    }

    Json parse_number() {
        size_t start = i;
        if (peek() == '-') i++;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '+' ||
                                s[i] == '-'))
            i++;
        if (i == start) { ok = false; return Json::null(); }
        return Json::number(std::strtod(s.c_str() + start, nullptr));
    }

    Json parse_array() {
        Json a = Json::array();
        i++; // [
        ws();
        if (peek() == ']') { i++; return a; }
        while (ok) {
            a.arr.push_back(parse_value());
            ws();
            if (peek() == ',') { i++; continue; }
            if (peek() == ']') { i++; break; }
            ok = false;
        }
        return a;
    }

    Json parse_object() {
        Json o = Json::object();
        i++; // {
        ws();
        if (peek() == '}') { i++; return o; }
        while (ok) {
            ws();
            if (peek() != '"') { ok = false; break; }
            std::string key = parse_string();
            ws();
            if (peek() != ':') { ok = false; break; }
            i++;
            o.obj.emplace_back(std::move(key), parse_value());
            ws();
            if (peek() == ',') { i++; continue; }
            if (peek() == '}') { i++; break; }
            ok = false;
        }
        return o;
    }
};

} // namespace

std::string Json::dump() const {
    std::string out;
    dump_to(out, *this);
    return out;
}

Json json_parse(const std::string& text, bool* ok_out) {
    Parser p(text);
    Json v = p.parse_value();
    if (ok_out) *ok_out = p.ok;
    return v;
}

} // namespace beans
