#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace beans {

// A tiny JSON value for the language server's JSON-RPC. Object members keep
// insertion order (LSP messages are small, so a vector is fine). Hand-written
// to keep the toolchain dependency-free.
struct Json {
    enum class T { null, boolean, number, string, array, object };
    T t = T::null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() = default;
    static Json null() { return Json{}; }
    static Json boolean(bool v) { Json j; j.t = T::boolean; j.b = v; return j; }
    static Json number(double v) { Json j; j.t = T::number; j.num = v; return j; }
    static Json string(std::string v) { Json j; j.t = T::string; j.str = std::move(v); return j; }
    static Json array() { Json j; j.t = T::array; return j; }
    static Json object() { Json j; j.t = T::object; return j; }

    bool is_null() const { return t == T::null; }
    bool is_object() const { return t == T::object; }
    bool is_array() const { return t == T::array; }
    bool is_string() const { return t == T::string; }
    bool is_number() const { return t == T::number; }

    // object helpers
    void set(std::string key, Json v) { obj.emplace_back(std::move(key), std::move(v)); }
    const Json* find(const std::string& key) const {
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
    // convenience readers (return defaults when absent / wrong type)
    std::string get_str(const std::string& key, const std::string& def = "") const {
        const Json* j = find(key);
        return j && j->t == T::string ? j->str : def;
    }
    double get_num(const std::string& key, double def = 0) const {
        const Json* j = find(key);
        return j && j->t == T::number ? j->num : def;
    }
    // array push
    void push(Json v) { arr.push_back(std::move(v)); }

    std::string dump() const; // serialize
};

// Parse a JSON document. On error returns a null Json and sets *ok=false.
Json json_parse(const std::string& text, bool* ok = nullptr);

} // namespace beans
