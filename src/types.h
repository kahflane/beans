#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace beans {

// Semantic types, interned in a TypePool — equal types share one TypeId,
// so type equality is pointer equality.
struct Type;
using TypeId = const Type*;

struct Type {
    enum class K {
        // numerics — int/i64, float/f64, byte/u8 are folded together
        int_, i8, i16, i32, u8, u16, u32, u64_, f32, f64_, decimal_,
        bool_, string_,
        unit,     // "no return value"
        poison,   // an error was already reported — stops cascades
        class_,   // user class/interface or builtin container: name + args
        enum_,    // user enum, or Option/Result: name + args
        type_param,
        fn,
        range,    // args[0] = element type
        package,  // imported package name
    };

    K k;
    std::string name;          // class_/enum_/type_param/package
    std::vector<TypeId> args;  // generic args / range element
    std::vector<TypeId> fn_params;
    TypeId fn_ret = nullptr;

    bool is_int() const {
        switch (k) {
            case K::int_: case K::i8: case K::i16: case K::i32:
            case K::u8: case K::u16: case K::u32: case K::u64_:
                return true;
            default: return false;
        }
    }
    bool is_float() const { return k == K::f32 || k == K::f64_; }
    bool is_numeric() const { return is_int() || is_float() || k == K::decimal_; }
};

std::string type_name(TypeId t);

class TypePool {
public:
    TypeId prim(Type::K k) {
        Type t;
        t.k = k;
        return intern(t);
    }
    TypeId named(Type::K k, std::string name, std::vector<TypeId> args = {}) {
        Type t;
        t.k = k;
        t.name = std::move(name);
        t.args = std::move(args);
        return intern(t);
    }
    TypeId fn(std::vector<TypeId> params, TypeId ret) {
        Type t;
        t.k = Type::K::fn;
        t.fn_params = std::move(params);
        t.fn_ret = ret;
        return intern(t);
    }
    TypeId range_of(TypeId elem) {
        Type t;
        t.k = Type::K::range;
        t.args = {elem};
        return intern(t);
    }

private:
    TypeId intern(const Type& t) {
        std::string key = make_key(t);
        auto it = by_key_.find(key);
        if (it != by_key_.end()) return it->second;
        auto owned = std::make_unique<Type>(t);
        TypeId id = owned.get();
        by_key_.emplace(std::move(key), id);
        storage_.push_back(std::move(owned));
        return id;
    }

    static std::string make_key(const Type& t) {
        std::string k = std::to_string(static_cast<int>(t.k)) + ":" + t.name;
        for (TypeId a : t.args) k += "," + std::to_string(reinterpret_cast<uintptr_t>(a));
        k += ";";
        for (TypeId p : t.fn_params) k += "," + std::to_string(reinterpret_cast<uintptr_t>(p));
        k += "->" + std::to_string(reinterpret_cast<uintptr_t>(t.fn_ret));
        return k;
    }

    std::map<std::string, TypeId> by_key_;
    std::vector<std::unique_ptr<Type>> storage_;
};

} // namespace beans
