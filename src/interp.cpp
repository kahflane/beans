#include "interp.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <spawn.h>
#include <set>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "builtins.h"
#include "c_abi.h"
#include "lexer.h"
#include "parser.h"

extern "C" char** environ;

namespace beans {

static std::atomic<int> g_live_threads{0};
void beans_threads_inc() { g_live_threads.fetch_add(1, std::memory_order_relaxed); }
void beans_threads_dec() { g_live_threads.fetch_sub(1, std::memory_order_relaxed); }
bool beans_threads_live() { return g_live_threads.load(std::memory_order_relaxed) > 0; }

static constexpr size_t MAP_LINEAR_MAX = 8;
static constexpr uint64_t IDX_POS = 0xffffffffull;
static constexpr uint64_t IDX_FRAG = ~IDX_POS;

namespace {

using CallbackDispatch = void (*)(void*, void*, void**);
using AggregateBridge = void (*)(void*, void*, void**, CallbackDispatch, void**);

AggregateBridge load_aggregate_bridge(const std::string& source,
                                      std::string& error) {
    struct Entry {
        void* handle = nullptr;
        AggregateBridge call = nullptr;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    static std::atomic<unsigned long long> sequence{0};

    std::lock_guard<std::mutex> lock(mutex);
    auto found = cache.find(source);
    if (found != cache.end()) return found->second.call;

    char directory[] = "/tmp/beans-ffi-XXXXXX";
    if (!mkdtemp(directory)) {
        error = "cannot create temporary directory for C ABI bridge";
        return nullptr;
    }
    std::string stem = std::string(directory) + "/bridge_" +
                       std::to_string(sequence.fetch_add(1));
    std::string c_path = stem + ".c";
#ifdef __APPLE__
    std::string lib_path = stem + ".dylib";
#else
    std::string lib_path = stem + ".so";
#endif
    {
        std::ofstream output(c_path);
        if (!output || !(output << source)) {
            error = "cannot write C ABI bridge source";
            rmdir(directory);
            return nullptr;
        }
    }

#ifdef __APPLE__
    std::vector<char*> argv = {
        const_cast<char*>("clang"), const_cast<char*>("-O2"),
        const_cast<char*>("-dynamiclib"), const_cast<char*>(c_path.c_str()),
        const_cast<char*>("-o"), const_cast<char*>(lib_path.c_str()), nullptr,
    };
#else
    std::vector<char*> argv = {
        const_cast<char*>("clang"), const_cast<char*>("-O2"),
        const_cast<char*>("-shared"), const_cast<char*>("-fPIC"),
        const_cast<char*>(c_path.c_str()), const_cast<char*>("-o"),
        const_cast<char*>(lib_path.c_str()), nullptr,
    };
#endif
    pid_t child = 0;
    int spawn_status = posix_spawnp(&child, "clang", nullptr, nullptr, argv.data(),
                                    environ);
    int status = 0;
    if (spawn_status != 0 || waitpid(child, &status, 0) < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "clang could not build the C ABI bridge";
        std::remove(c_path.c_str());
        std::remove(lib_path.c_str());
        rmdir(directory);
        return nullptr;
    }

    void* handle = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    AggregateBridge call = handle ? reinterpret_cast<AggregateBridge>(
                                        dlsym(handle, "beans_ffi_bridge"))
                                  : nullptr;
    if (!call) {
        const char* message = dlerror();
        error = message ? message : "cannot load C ABI bridge";
        if (handle) dlclose(handle);
    }
    std::remove(c_path.c_str());
    std::remove(lib_path.c_str());
    rmdir(directory);
    if (!call) return nullptr;
    cache.emplace(source, Entry{handle, call});
    return call;
}

// per-thread frame state: defers + the return-type hint of the running fn
struct Frame {
    std::vector<std::pair<const Expr*, std::shared_ptr<Env>>> defers;
    const TypeRef* ret = nullptr;
};

thread_local std::vector<Frame> g_frames;

int64_t parse_int_text(std::string_view text) {
    std::string clean;
    for (char c : text) {
        if (c != '_') clean.push_back(c);
    }
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 16));
    }
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B')) {
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 2));
    }
    return static_cast<int64_t>(std::strtoull(clean.c_str(), nullptr, 10));
}

std::string clean_number(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

template <typename T>
uint64_t atomic_load_raw(void* address) {
    return static_cast<uint64_t>(std::atomic_ref<T>(*static_cast<T*>(address)).load());
}

template <typename T>
void atomic_store_raw(void* address, uint64_t value) {
    std::atomic_ref<T>(*static_cast<T*>(address)).store(static_cast<T>(value));
}

template <typename T>
uint64_t atomic_fetch_add_raw(void* address, uint64_t value) {
    return static_cast<uint64_t>(
        std::atomic_ref<T>(*static_cast<T*>(address)).fetch_add(static_cast<T>(value)));
}

template <typename T>
bool atomic_compare_exchange_raw(void* address, uint64_t expected, uint64_t desired) {
    T old = static_cast<T>(expected);
    return std::atomic_ref<T>(*static_cast<T*>(address))
        .compare_exchange_strong(old, static_cast<T>(desired));
}

uint64_t raw_atomic_load(const RawPtrVal& pointer) {
    switch (pointer.size) {
        case 1: return atomic_load_raw<uint8_t>(pointer.address);
        case 2: return atomic_load_raw<uint16_t>(pointer.address);
        case 4: return atomic_load_raw<uint32_t>(pointer.address);
        default: return atomic_load_raw<uint64_t>(pointer.address);
    }
}

void raw_atomic_store(const RawPtrVal& pointer, uint64_t value) {
    switch (pointer.size) {
        case 1: atomic_store_raw<uint8_t>(pointer.address, value); break;
        case 2: atomic_store_raw<uint16_t>(pointer.address, value); break;
        case 4: atomic_store_raw<uint32_t>(pointer.address, value); break;
        default: atomic_store_raw<uint64_t>(pointer.address, value); break;
    }
}

uint64_t raw_atomic_fetch_add(const RawPtrVal& pointer, uint64_t value) {
    switch (pointer.size) {
        case 1: return atomic_fetch_add_raw<uint8_t>(pointer.address, value);
        case 2: return atomic_fetch_add_raw<uint16_t>(pointer.address, value);
        case 4: return atomic_fetch_add_raw<uint32_t>(pointer.address, value);
        default: return atomic_fetch_add_raw<uint64_t>(pointer.address, value);
    }
}

bool raw_atomic_compare_exchange(const RawPtrVal& pointer, uint64_t expected,
                                 uint64_t desired) {
    switch (pointer.size) {
        case 1:
            return atomic_compare_exchange_raw<uint8_t>(pointer.address, expected, desired);
        case 2:
            return atomic_compare_exchange_raw<uint16_t>(pointer.address, expected, desired);
        case 4:
            return atomic_compare_exchange_raw<uint32_t>(pointer.address, expected, desired);
        default:
            return atomic_compare_exchange_raw<uint64_t>(pointer.address, expected, desired);
    }
}

template <typename Ret, size_t Remaining, typename... Packed>
Ret call_c_abi_tail(void* symbol, const std::vector<Value>& args,
                    const uint64_t* words, size_t index, Packed... packed) {
    if constexpr (Remaining == 0) {
        using Function = Ret (*)(Packed...);
        if constexpr (std::is_void_v<Ret>) {
            reinterpret_cast<Function>(symbol)(packed...);
        } else {
            return reinterpret_cast<Function>(symbol)(packed...);
        }
    } else {
        if (args[index].k == Value::K::float_) {
            if (args[index].float_bits == 32) {
                return call_c_abi_tail<Ret, Remaining - 1, Packed..., float>(
                    symbol, args, words, index + 1, packed...,
                    static_cast<float>(args[index].f));
            }
            return call_c_abi_tail<Ret, Remaining - 1, Packed..., double>(
                symbol, args, words, index + 1, packed..., args[index].f);
        }
        return call_c_abi_tail<Ret, Remaining - 1, Packed..., uint64_t>(
            symbol, args, words, index + 1, packed..., words[index]);
    }
}

template <typename Ret>
Ret call_c_abi(void* symbol, const std::vector<Value>& args, const uint64_t* words) {
    switch (args.size()) {
        case 0: return call_c_abi_tail<Ret, 0>(symbol, args, words, 0);
        case 1: return call_c_abi_tail<Ret, 1>(symbol, args, words, 0);
        case 2: return call_c_abi_tail<Ret, 2>(symbol, args, words, 0);
        case 3: return call_c_abi_tail<Ret, 3>(symbol, args, words, 0);
        case 4: return call_c_abi_tail<Ret, 4>(symbol, args, words, 0);
        case 5: return call_c_abi_tail<Ret, 5>(symbol, args, words, 0);
        default: return call_c_abi_tail<Ret, 6>(symbol, args, words, 0);
    }
}

} // namespace

// ---- hints -----------------------------------------------------------------

Interp::Hint Interp::Hint::of(const TypeRef* t) {
    Hint h;
    h.tref = t;
    if (t && t->kind == TypeRef::Kind::named) {
        if (t->name == "decimal") h.num = NumHint::dec;
        else if (t->name == "float" || t->name == "f64" || t->name == "f32")
            h.num = NumHint::flt;
    }
    return h;
}
bool Interp::Hint::wants_dec() const { return num == NumHint::dec; }
bool Interp::Hint::wants_float() const { return num == NumHint::flt; }
const TypeRef* Interp::Hint::arg(size_t i) const {
    if (tref && tref->kind == TypeRef::Kind::fixed_array && i == 0)
        return tref->array_elem.get();
    if (tref && tref->kind == TypeRef::Kind::named && i < tref->args.size()) {
        return tref->args[i].get();
    }
    return nullptr;
}

int64_t Interp::signed_integer(uint64_t bits) {
    int64_t value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

uint64_t Interp::unsigned_integer(const Value& value) {
    uint64_t bits;
    std::memcpy(&bits, &value.i, sizeof bits);
    if (value.int_bits < 64) bits &= (uint64_t{1} << value.int_bits) - 1;
    return bits;
}

void Interp::normalize_integer(Value& value) {
    uint64_t bits = unsigned_integer(value);
    if (value.int_bits < 64) {
        uint64_t mask = (uint64_t{1} << value.int_bits) - 1;
        bits &= mask;
        if (!value.int_unsigned && (bits & (uint64_t{1} << (value.int_bits - 1))))
            bits |= ~mask;
    }
    value.i = signed_integer(bits);
}

void Interp::normalize_float(Value& value) {
    if (value.float_bits == 32) value.f = static_cast<double>(static_cast<float>(value.f));
}

Value Interp::typed_integer(int64_t bits, TypeId type, const TypeRef* fallback) const {
    Value value = Value::of_int(bits);
    IntLayout layout = type ? hir_.target().integer(type->k) : IntLayout{};
    if (!layout.bits && fallback && fallback->kind == TypeRef::Kind::named) {
        const std::string& name = fallback->name;
        if (name == "i8") layout = {8, true};
        else if (name == "i16") layout = {16, true};
        else if (name == "i32") layout = {32, true};
        else if (name == "int" || name == "i64") layout = {64, true};
        else if (name == "u8" || name == "byte") layout = {8, false};
        else if (name == "u16") layout = {16, false};
        else if (name == "u32") layout = {32, false};
        else if (name == "u64") layout = {64, false};
    }
    if (layout.bits) {
        value.int_bits = layout.bits;
        value.int_unsigned = !layout.is_signed;
        normalize_integer(value);
    }
    return value;
}

Value Interp::typed_float(double number, TypeId type, const TypeRef* fallback) const {
    Value value = Value::of_float(number);
    uint8_t bits = type ? hir_.target().float_bits(type->k) : 0;
    if (!bits && fallback && fallback->kind == TypeRef::Kind::named)
        bits = fallback->name == "f32" ? 32 : 64;
    if (bits) value.float_bits = bits;
    normalize_float(value);
    return value;
}

RawPtrVal Interp::raw_spec(TypeId pointer_type) const {
    if (!pointer_type || pointer_type->k != Type::K::class_ ||
        pointer_type->name != "RawPtr" || pointer_type->args.size() != 1) {
        return {};
    }
    return raw_value_spec(pointer_type->args[0]);
}

RawPtrVal Interp::raw_value_spec(TypeId type) const {
    RawPtrVal spec;
    if (!type) return spec;
    IntLayout integer = hir_.target().integer(type->k);
    if (integer.bits) {
        spec.scalar = integer.is_signed ? RawScalar::signed_int : RawScalar::unsigned_int;
        spec.bits = integer.bits;
        spec.size = static_cast<uint8_t>(integer.bits / 8);
    } else if (uint8_t bits = hir_.target().float_bits(type->k)) {
        spec.scalar = RawScalar::float_;
        spec.bits = bits;
        spec.size = static_cast<uint8_t>(bits / 8);
    } else if (type->k == Type::K::bool_) {
        spec.scalar = RawScalar::boolean;
        spec.bits = 1;
        spec.size = 1;
    } else if (type->k == Type::K::class_ && type->name == "RawPtr" &&
               type->args.size() == 1) {
        spec.scalar = RawScalar::pointer;
        spec.size = static_cast<uint32_t>(sizeof(void*));
        spec.align = static_cast<uint32_t>(alignof(void*));
        spec.element = std::make_shared<RawPtrVal>(raw_value_spec(type->args[0]));
        return spec;
    } else if (type->k == Type::K::fixed_array && type->args.size() == 1) {
        spec.scalar = RawScalar::array;
        spec.element = std::make_shared<RawPtrVal>(raw_value_spec(type->args[0]));
        spec.element_count = static_cast<uint32_t>(std::stoul(type->name));
        spec.align = spec.element->align;
        spec.size = spec.element->size * spec.element_count;
        return spec;
    } else if (type->k == Type::K::struct_) {
        const ClassDecl* decl = find_class(type->name);
        if (decl && (decl->is_struct || decl->is_union) && decl->is_c_layout) {
            spec.scalar = RawScalar::record;
            spec.record_decl = decl;
            spec.align = 1;
            uint32_t offset = 0;
            for (const FieldDecl& field : decl->fields) {
                RawPtrVal value = raw_value_spec(field.type.get());
                if (decl->is_union) {
                    if (value.size > offset) offset = value.size;
                } else {
                    offset = (offset + value.align - 1) & ~(value.align - 1);
                    offset += value.size;
                }
                if (value.align > spec.align) spec.align = value.align;
            }
            spec.size = (offset + spec.align - 1) & ~(spec.align - 1);
        }
    }
    if (spec.scalar != RawScalar::record) spec.align = spec.size ? spec.size : 1;
    return spec;
}

RawPtrVal Interp::raw_value_spec(const TypeRef* type) const {
    RawPtrVal spec;
    if (!type) return spec;
    if (type->kind == TypeRef::Kind::fixed_array && type->array_elem) {
        spec.scalar = RawScalar::array;
        spec.element = std::make_shared<RawPtrVal>(raw_value_spec(type->array_elem.get()));
        spec.element_count = type->array_len;
        spec.align = spec.element->align;
        spec.size = spec.element->size * spec.element_count;
        return spec;
    }
    if (type->kind != TypeRef::Kind::named) return spec;
    const std::string& name = type->name;
    if (name == "i8" || name == "i16" || name == "i32" || name == "i64" ||
        name == "int") {
        spec.scalar = RawScalar::signed_int;
        spec.bits = name == "i8" ? 8 : name == "i16" ? 16 : name == "i32" ? 32 : 64;
    } else if (name == "u8" || name == "byte" || name == "u16" || name == "u32" ||
               name == "u64") {
        spec.scalar = RawScalar::unsigned_int;
        spec.bits = (name == "u8" || name == "byte") ? 8
                    : name == "u16"                  ? 16
                    : name == "u32"                  ? 32
                                                       : 64;
    } else if (name == "f32" || name == "f64" || name == "float") {
        spec.scalar = RawScalar::float_;
        spec.bits = name == "f32" ? 32 : 64;
    } else if (name == "bool") {
        spec.scalar = RawScalar::boolean;
        spec.bits = 1;
    } else if (name == "RawPtr" && type->args.size() == 1) {
        spec.scalar = RawScalar::pointer;
        spec.size = static_cast<uint32_t>(sizeof(void*));
        spec.align = static_cast<uint32_t>(alignof(void*));
        spec.element = std::make_shared<RawPtrVal>(raw_value_spec(type->args[0].get()));
        return spec;
    } else {
        std::string key = type->resolved.empty() ? qual(name) : type->resolved;
        const ClassDecl* decl = find_class(key);
        if (!decl && type->resolved.empty()) decl = find_class(name);
        if (decl && (decl->is_struct || decl->is_union) && decl->is_c_layout) {
            spec.scalar = RawScalar::record;
            spec.record_decl = decl;
            spec.align = 1;
            uint32_t offset = 0;
            for (const FieldDecl& field : decl->fields) {
                RawPtrVal value = raw_value_spec(field.type.get());
                if (decl->is_union) {
                    if (value.size > offset) offset = value.size;
                } else {
                    offset = (offset + value.align - 1) & ~(value.align - 1);
                    offset += value.size;
                }
                if (value.align > spec.align) spec.align = value.align;
            }
            spec.size = (offset + spec.align - 1) & ~(spec.align - 1);
            return spec;
        }
    }
    spec.size = spec.bits == 1 ? 1 : static_cast<uint8_t>(spec.bits / 8);
    spec.align = spec.size ? spec.size : 1;
    return spec;
}

RawPtrVal Interp::raw_spec(const TypeRef* pointer_type) const {
    if (!pointer_type || pointer_type->kind != TypeRef::Kind::named ||
        pointer_type->name != "RawPtr" || pointer_type->args.size() != 1) {
        return {};
    }
    return raw_value_spec(pointer_type->args[0].get());
}

Value Interp::raw_read(const RawPtrVal& pointer) const {
    if (pointer.scalar == RawScalar::array && pointer.element) {
        Value out;
        out.k = Value::K::fixed_array;
        out.array.reserve(pointer.element_count);
        for (uint32_t i = 0; i < pointer.element_count; i++) {
            RawPtrVal element = *pointer.element;
            element.address = static_cast<char*>(pointer.address) + i * element.size;
            out.array.push_back(raw_read(element));
        }
        return out;
    }
    if (pointer.scalar == RawScalar::record && pointer.record_decl) {
        Value out;
        out.k = Value::K::struct_v;
        out.struct_decl = pointer.record_decl;
        if (pointer.record_decl->is_union) {
            out.union_bytes.resize(pointer.size);
            std::memcpy(out.union_bytes.data(), pointer.address, pointer.size);
            return out;
        }
        uint32_t offset = 0;
        for (const FieldDecl& field : pointer.record_decl->fields) {
            RawPtrVal value = raw_value_spec(field.type.get());
            offset = (offset + value.align - 1) & ~(value.align - 1);
            value.address = static_cast<char*>(pointer.address) + offset;
            out.struct_fields.push_back(raw_read(value));
            offset += value.size;
        }
        return out;
    }
    if (pointer.scalar == RawScalar::pointer && pointer.element) {
        void* address = nullptr;
        std::memcpy(&address, pointer.address, sizeof address);
        Value out;
        out.k = Value::K::rawptr;
        out.raw = *pointer.element;
        out.raw.address = address;
        return out;
    }
    if (pointer.scalar == RawScalar::float_) {
        if (pointer.bits == 32) {
            float value = 0;
            std::memcpy(&value, pointer.address, sizeof value);
            Value out = Value::of_float(value);
            out.float_bits = 32;
            return out;
        }
        double value = 0;
        std::memcpy(&value, pointer.address, sizeof value);
        return Value::of_float(value);
    }
    if (pointer.scalar == RawScalar::boolean) {
        uint8_t value = 0;
        std::memcpy(&value, pointer.address, 1);
        return Value::of_bool(value != 0);
    }
    uint64_t bits = 0;
    std::memcpy(&bits, pointer.address, pointer.size);
    Value out = Value::of_int(signed_integer(bits));
    out.int_bits = pointer.bits;
    out.int_unsigned = pointer.scalar == RawScalar::unsigned_int;
    normalize_integer(out);
    return out;
}

void Interp::raw_write(const RawPtrVal& pointer, const Value& value) const {
    if (pointer.scalar == RawScalar::array && pointer.element) {
        for (uint32_t i = 0; i < pointer.element_count; i++) {
            RawPtrVal element = *pointer.element;
            element.address = static_cast<char*>(pointer.address) + i * element.size;
            raw_write(element, value.array[i]);
        }
        return;
    }
    if (pointer.scalar == RawScalar::record && pointer.record_decl) {
        if (pointer.record_decl->is_union) {
            std::memcpy(pointer.address, value.union_bytes.data(), pointer.size);
            return;
        }
        uint32_t offset = 0;
        for (size_t i = 0; i < pointer.record_decl->fields.size(); i++) {
            RawPtrVal field = raw_value_spec(pointer.record_decl->fields[i].type.get());
            offset = (offset + field.align - 1) & ~(field.align - 1);
            field.address = static_cast<char*>(pointer.address) + offset;
            raw_write(field, value.struct_fields[i]);
            offset += field.size;
        }
        return;
    }
    if (pointer.scalar == RawScalar::pointer) {
        void* address = value.raw.address;
        std::memcpy(pointer.address, &address, sizeof address);
        return;
    }
    if (pointer.scalar == RawScalar::float_) {
        if (pointer.bits == 32) {
            float stored = static_cast<float>(value.f);
            std::memcpy(pointer.address, &stored, sizeof stored);
        } else {
            std::memcpy(pointer.address, &value.f, sizeof value.f);
        }
        return;
    }
    if (pointer.scalar == RawScalar::boolean) {
        uint8_t stored = value.b ? 1 : 0;
        std::memcpy(pointer.address, &stored, 1);
        return;
    }
    uint64_t stored = unsigned_integer(value);
    std::memcpy(pointer.address, &stored, pointer.size);
}

// ---- setup -----------------------------------------------------------------

// which package's code this thread is currently running (parallel to g_frames)
thread_local std::string g_pkg;

// the interp running on this thread — the instance deleter needs it to run
// deinit, and a deleter can fire on whichever thread drops the last reference
static thread_local Interp* g_interp_tl = nullptr;

// deleter for instances of classes with a deinit anywhere in their chain:
// user code runs first (fields still alive), then the normal teardown
struct DeinitDeleter {
    void operator()(InstanceVal* p) const {
        if (g_interp_tl && !g_beans_panicking) g_interp_tl->run_deinit(p);
        delete p;
    }
};

Interp::Interp(const HirProgram& hir) : hir_(hir) {
    const Program& prog = hir.ast();
    for (const auto& pkg : prog.packages) {
        prefix_by_path_[pkg->import_path] = pkg->prefix;
        auto& bindings = pkg_imports_[pkg->prefix];
        for (const auto& pf : pkg->files) {
            for (const ClassDecl& c : pf->mod.classes) classes_[c.qualname] = &c;
            for (const EnumDecl& e : pf->mod.enums) enums_[e.qualname] = &e;
            for (const FnDecl& f : pf->mod.fns) fns_[f.qualname] = &f;
            for (const ImportDecl& i : pf->mod.imports) {
                std::string bound = i.alias;
                if (bound.empty()) {
                    size_t cut = i.path.find_last_of("./");
                    bound = cut == std::string::npos ? i.path : i.path.substr(cut + 1);
                }
                bindings[bound] = i.path;
            }
        }
    }
}

std::string Interp::qual(const std::string& name) const {
    return g_pkg.empty() ? name : g_pkg + "." + name;
}

std::string Interp::pkg_of(const std::string& qualname) {
    size_t dot = qualname.find('.');
    return dot == std::string::npos ? "" : qualname.substr(0, dot);
}

const ClassDecl* Interp::resolve_class(const Expr* annotated, const std::string& name) const {
    if (annotated && !annotated->resolved.empty()) return find_class(annotated->resolved);
    if (const ClassDecl* c = find_class(qual(name))) return c;
    return nullptr;
}

const EnumDecl* Interp::resolve_enum(const Expr* annotated, const std::string& name) const {
    const std::string& key = annotated && !annotated->resolved.empty()
                                 ? annotated->resolved
                                 : qual(name);
    auto it = enums_.find(key);
    return it == enums_.end() ? nullptr : it->second;
}

std::string Interp::binding_path(const std::string& binding) const {
    auto pit = pkg_imports_.find(g_pkg);
    if (pit == pkg_imports_.end()) return "";
    auto it = pit->second.find(binding);
    return it == pit->second.end() ? "" : it->second;
}

int Interp::run() {
    g_interp_tl = this;
    auto it = fns_.find("main");
    if (it == fns_.end()) {
        std::fprintf(stderr, "error: no fn main\n");
        return 2;
    }
    try {
        call_fn(it->second, nullptr, {}, "");
    } catch (const BeansPanic& p) {
        std::fflush(stdout); // buffered stdout before the stderr panic line
        std::fprintf(stderr, "runtime panic at %u:%u: %s\n", p.line, p.col, p.msg.c_str());
        return 3;
    } catch (const std::bad_alloc&) {
        // same bytes the native runtime prints when calloc gives up
        std::fflush(stdout);
        std::fprintf(stderr, "runtime panic at 0:0: out of memory\n");
        return 3;
    } catch (const std::length_error&) {
        std::fflush(stdout);
        std::fprintf(stderr, "runtime panic at 0:0: out of memory\n");
        return 3;
    }
    return 0;
}

void Interp::panic(const Expr* e, std::string msg) {
    g_beans_panicking = true; // suppress deinit while this unwinds
    BeansPanic p;
    p.msg = std::move(msg);
    if (e) { p.line = e->line; p.col = e->col; }
    throw p;
}

Value Interp::some(Value v) {
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Option";
    x.en->variant = "some";
    x.en->payload.push_back(std::move(v));
    return x;
}
Value Interp::none() {
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Option";
    x.en->variant = "none";
    return x;
}
Value Interp::make_err(std::string msg) {
    Value e;
    e.k = Value::K::instance;
    e.inst = std::make_shared<InstanceVal>();
    e.inst->fields.emplace_back("msg", Value::of_str(std::move(msg)));
    e.inst->fields.emplace_back("kind", Value::of_str(""));
    return e;
}

// ---- class helpers ----------------------------------------------------------

const ClassDecl* Interp::find_class(const std::string& name) const {
    auto it = classes_.find(name);
    return it == classes_.end() ? nullptr : it->second;
}

// parents as qualified keys (the checker pinned them); raw names as fallback
static std::vector<std::string> supers_of(const ClassDecl* c) {
    std::vector<std::string> out;
    std::string base = c->base_resolved.empty() ? c->base : c->base_resolved;
    if (!base.empty()) out.push_back(std::move(base));
    const std::vector<std::string>& interfaces =
        c->interfaces_resolved.size() == c->interfaces.size()
            ? c->interfaces_resolved
            : c->interfaces;
    out.insert(out.end(), interfaces.begin(), interfaces.end());
    return out;
}

const FnDecl* Interp::find_method(const ClassDecl* cls, const std::string& name,
                                  const ClassDecl** owner) const {
    std::vector<const ClassDecl*> work = {cls};
    std::vector<const ClassDecl*> seen;
    while (!work.empty()) {
        const ClassDecl* c = work.front();
        work.erase(work.begin());
        if (!c) continue;
        bool dup = false;
        for (const ClassDecl* s : seen) dup |= s == c;
        if (dup) continue;
        seen.push_back(c);
        for (const FnDecl& m : c->methods) {
            if (m.name == name && m.has_body) {
                if (owner) *owner = c; // inherited code runs as its own package
                return &m;
            }
        }
        for (const std::string& s : supers_of(c)) work.push_back(find_class(s));
    }
    return nullptr;
}

bool Interp::class_is(const ClassDecl* cls, const std::string& super) const {
    if (!cls) return false;
    if (cls->qualname == super) return true;
    for (const std::string& s : supers_of(cls)) {
        if (class_is(find_class(s), super)) return true;
    }
    return false;
}

void Interp::collect_fields(const ClassDecl* cls, std::vector<const FieldDecl*>& out) const {
    if (!cls) return;
    for (const std::string& s : supers_of(cls)) collect_fields(find_class(s), out);
    for (const FieldDecl& f : cls->fields) {
        bool have = false;
        for (const FieldDecl* g : out) have |= g->name == f.name;
        if (!have) out.push_back(&f);
    }
}

Value Interp::make_instance(const ClassDecl* cls,
                            const std::vector<InitEntry>& entries,
                            std::shared_ptr<Env>& env) {
    Value v;
    if (cls->is_union) {
        v.k = Value::K::struct_v;
        v.struct_decl = cls;
        uint32_t size = 0, align = 1;
        for (const FieldDecl& field : cls->fields) {
            RawPtrVal spec = raw_value_spec(field.type.get());
            if (spec.size > size) size = spec.size;
            if (spec.align > align) align = spec.align;
        }
        size = (size + align - 1) & ~(align - 1);
        v.union_bytes.assign(size, 0);
        for (const InitEntry& entry : entries) {
            for (const FieldDecl& field : cls->fields) {
                if (field.name != entry.name) continue;
                Value init = eval(entry.value.get(), env, Hint::of(field.type.get()));
                RawPtrVal spec = raw_value_spec(field.type.get());
                spec.address = v.union_bytes.data();
                raw_write(spec, init);
            }
        }
        return v;
    }
    if (cls->is_struct) {
        v.k = Value::K::struct_v;
        v.struct_decl = cls;
        std::string saved_pkg = g_pkg;
        g_pkg = pkg_of(cls->qualname);
        for (const FieldDecl& field : cls->fields) {
            Value init = Value::unit();
            if (field.def) {
                std::shared_ptr<Env> empty = std::make_shared<Env>();
                init = eval(field.def.get(), empty, Hint::of(field.type.get()));
            }
            v.struct_fields.push_back(std::move(init));
        }
        g_pkg = saved_pkg;
        for (const InitEntry& entry : entries) {
            for (size_t i = 0; i < cls->fields.size(); i++) {
                if (cls->fields[i].name != entry.name) continue;
                v.struct_fields[i] = eval(entry.value.get(), env,
                                          Hint::of(cls->fields[i].type.get()));
            }
        }
        return v;
    }
    v.k = Value::K::instance;
    // classes with a deinit in their chain pay for the custom deleter;
    // everything else keeps the plain single-allocation shared_ptr
    v.inst = needs_deinit(cls) ? std::shared_ptr<InstanceVal>(new InstanceVal, DeinitDeleter{})
                               : std::make_shared<InstanceVal>();
    v.inst->cls = cls;

    std::vector<const FieldDecl*> fields;
    collect_fields(cls, fields);
    std::string saved_pkg = g_pkg;
    g_pkg = pkg_of(cls->qualname); // defaults are the class's own code
    for (const FieldDecl* f : fields) {
        Value init = Value::unit();
        if (f->def) {
            std::shared_ptr<Env> empty = std::make_shared<Env>();
            init = eval(f->def.get(), empty, Hint::of(f->type.get()));
        }
        v.inst->fields.emplace_back(f->name, std::move(init));
    }
    g_pkg = saved_pkg;
    for (const InitEntry& en : entries) {
        const FieldDecl* fd = nullptr;
        for (const FieldDecl* f : fields) {
            if (f->name == en.name) fd = f;
        }
        Value val = eval(en.value.get(), env, fd ? Hint::of(fd->type.get()) : Hint());
        if (Value* slot = v.inst->field(en.name)) *slot = std::move(val);
    }
    return v;
}

// ---- init / deinit ----------------------------------------------------------

const ClassDecl* Interp::parent_of(const ClassDecl* c) const {
    std::string name = c->base_resolved.empty() ? c->base : c->base_resolved;
    if (!name.empty()) return find_class(name);
    return nullptr;
}

static const FnDecl* own_deinit(const ClassDecl* c) {
    for (const FnDecl& m : c->methods) {
        if (m.has_self && m.name == "deinit") return &m;
    }
    return nullptr;
}

bool Interp::needs_deinit(const ClassDecl* c) const {
    for (const ClassDecl* k = c; k; k = parent_of(k)) {
        if (own_deinit(k)) return true;
    }
    return false;
}

Value Interp::construct(const ClassDecl* cls, const Expr* e, std::shared_ptr<Env>& env) {
    // the constructor may be inherited: nearest class up the chain with an
    // init builds this class (anything between adds no required fields)
    const FnDecl* ini = nullptr;
    const ClassDecl* owner = nullptr;
    for (const ClassDecl* k = cls; k && !ini; k = parent_of(k)) {
        for (const FnDecl& m : k->methods) {
            if (m.has_self && m.name == "init") { ini = &m; owner = k; }
        }
    }
    if (!ini) return make_instance(cls, {}, env); // compiler-provided init()
    std::vector<Value> args;
    for (size_t i = 0; i < e->args.size(); i++) {
        Hint h = i < ini->params.size() ? Hint::of(ini->params[i].type.get()) : Hint();
        args.push_back(eval(e->args[i].get(), env, h));
    }
    Value v = make_instance(cls, {}, env); // cls, not owner: the child's layout
    call_fn(ini, &v, std::move(args), pkg_of(owner->qualname));
    return v;
}

Value Interp::eval_new(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    if (!e->resolved.empty()) {
        if (const ClassDecl* cls = find_class(e->resolved)) return construct(cls, e, env);
    }
    if (const ClassDecl* cls = find_class(e->name)) return construct(cls, e, env);
    if (const ClassDecl* cls = find_class(qual(e->name))) return construct(cls, e, env);

    const TypeRef* inner = !e->type_args.empty() ? e->type_args[0].get() : hint.arg(0);
    if (e->name == "Arena") {
        Value cap = eval(e->args[0].get(), env);
        if (cap.i < 0) panic(e, "negative arena capacity " + std::to_string(cap.i));
        if (cap.i > (1LL << 58)) panic(e, "arena capacity too large");
        Value v;
        v.k = Value::K::arena;
        v.arena = std::make_shared<ArenaVal>();
        v.arena->values.reserve(static_cast<size_t>(cap.i));
        return v;
    }
    if (e->name == "Box") {
        Value v;
        v.k = Value::K::box;
        v.box = std::make_shared<BoxVal>();
        v.box->inner = eval(e->args[0].get(), env, Hint::of(inner));
        return v;
    }
    if (e->name == "Shared") {
        Value v;
        v.k = Value::K::shared;
        v.shared = std::make_shared<SharedVal>();
        v.shared->inner = eval(e->args[0].get(), env, Hint::of(inner));
        return v;
    }
    if (e->name == "Mutex") {
        Value value = eval(e->args[0].get(), env, Hint::of(inner));
        Value v;
        v.k = Value::K::mutex;
        v.mutex = std::make_shared<MutexVal>();
        v.mutex->inner = std::make_shared<Value>(std::move(value));
        return v;
    }
    if (e->name == "Channel") {
        Value cap = eval(e->args[0].get(), env);
        Value v;
        v.k = Value::K::channel;
        v.chan = std::make_shared<ChannelVal>();
        v.chan->cap = cap.i > 0 ? static_cast<size_t>(cap.i) : 1;
        return v;
    }
    if (e->name == "AtomicInt") {
        Value initial = eval(e->args[0].get(), env);
        Value v;
        v.k = Value::K::atomic;
        v.atomic = std::make_shared<AtomicVal>();
        v.atomic->v = initial.i;
        return v;
    }
    if (e->name == "Bytes") {
        for (const BuiltinConstructor& builtin : builtin_constructors()) {
            if (std::string(builtin.cls) != "Bytes") continue;
            std::vector<Value> args;
            args.push_back(eval(e->args[0].get(), env));
            return builtin.run(e->line, e->col, args);
        }
    }
    panic(e, "cannot build '" + e->name + "'");
}

void Interp::run_deinit(InstanceVal* inst) {
    // non-owning alias: deinit sees self, and when the alias count hits zero
    // again nothing happens — the object is deleted right after regardless,
    // which is also why self must not escape (spec: use-after-free)
    Value self;
    self.k = Value::K::instance;
    self.inst = std::shared_ptr<InstanceVal>(inst, [](InstanceVal*) {});

    // deaths inside deinit must land immediately, like the native runtime
    // releasing at the deinit frame's exit — not queue behind the outer drain.
    // Recursion depth here is user deinit-nesting, same as native.
    bool saved = g_teardown.draining;
    g_teardown.draining = false;

    try {
        // subclass first, then up the chain — each class's own deinit once
        for (const ClassDecl* k = inst->cls; k; k = parent_of(k)) {
            if (const FnDecl* d = own_deinit(k)) {
                call_fn(d, &self, {}, pkg_of(k->qualname));
            }
        }
    } catch (const BeansPanic& p) {
        // fatal, exactly like the native runtime's beans_panic mid-release
        std::fflush(stdout);
        std::fprintf(stderr, "runtime panic at %u:%u: %s\n", p.line, p.col, p.msg.c_str());
        std::exit(3);
    }

    g_teardown.draining = saved;
}

// ---- calls -----------------------------------------------------------------

Value Interp::call_extern_aggregate(const FnDecl* fn,
                                    const std::vector<Value>& args,
                                    void* symbol) {
    auto fail = [&](const std::string& message) -> Value {
        g_beans_panicking = true;
        throw BeansPanic{message, fn->line, fn->col};
    };

    auto record_lookup = [&](const TypeRef* type) -> const ClassDecl* {
        if (!type || type->kind != TypeRef::Kind::named) return nullptr;
        std::string key = type->resolved.empty() ? qual(type->name) : type->resolved;
        const ClassDecl* record = find_class(key);
        if (!record && type->resolved.empty()) record = find_class(type->name);
        return record && (record->is_struct || record->is_union) && record->is_c_layout
                   ? record
                   : nullptr;
    };
    CAbiText abi = describe_c_abi(*fn, record_lookup);
    std::string source = "#include <stdint.h>\n" + abi.definitions;
    source += "typedef void (*BeansFfiDispatch)(void*, void*, void**);\n";
    for (const CAbiCallbackText& callback : abi.callbacks) {
        std::string prefix = "beans_ffi_cb" +
                             std::to_string(callback.parameter_index);
        source += "static _Thread_local BeansFfiDispatch " + prefix +
                  "_dispatch;\n";
        source += "static _Thread_local void* " + prefix + "_context;\n";
        source += "static " + callback.return_type + " " + prefix + "(";
        for (size_t i = 0; i < callback.parameter_declarations.size(); i++) {
            if (i) source += ", ";
            source += callback.parameter_declarations[i];
        }
        if (callback.parameter_declarations.empty()) source += "void";
        source += ") {\n  void* callback_args[" +
                  std::to_string(std::max<size_t>(callback.parameter_types.size(), 1)) +
                  "] = {";
        for (size_t i = 0; i < callback.parameter_types.size(); i++) {
            if (i) source += ", ";
            source += "&value" + std::to_string(i);
        }
        if (callback.parameter_types.empty()) source += "0";
        source += "};\n";
        if (callback.return_type == "void") {
            source += "  " + prefix + "_dispatch(" + prefix +
                      "_context, 0, callback_args);\n}\n";
        } else {
            source += "  " + callback.return_type +
                      " callback_result = {0};\n  " + prefix + "_dispatch(" +
                      prefix + "_context, &callback_result, callback_args);\n"
                      "  return callback_result;\n}\n";
        }
    }
    source += "typedef " + abi.return_type + " (*BeansFfiFn)(";
    for (size_t i = 0; i < abi.parameter_declarations.size(); i++) {
        if (i) source += ", ";
        source += abi.parameter_declarations[i];
    }
    if (abi.parameter_declarations.empty()) source += "void";
    source += ");\n__attribute__((visibility(\"default\")))\n";
    source += "void beans_ffi_bridge(void* symbol, void* result, void** args, "
              "BeansFfiDispatch dispatch, void** contexts) {\n";
    source += "  BeansFfiFn fn = (BeansFfiFn)symbol;\n  ";
    for (const CAbiCallbackText& callback : abi.callbacks) {
        std::string prefix = "beans_ffi_cb" +
                             std::to_string(callback.parameter_index);
        source += "BeansFfiDispatch " + prefix + "_old_dispatch = " + prefix +
                  "_dispatch;\n  void* " + prefix + "_old_context = " + prefix +
                  "_context;\n  " + prefix + "_dispatch = dispatch;\n  " + prefix +
                  "_context = contexts[" +
                  std::to_string(callback.parameter_index) + "];\n  ";
    }
    if (fn->ret) source += abi.return_type + " call_result = ";
    source += "fn(";
    for (size_t i = 0; i < fn->params.size(); i++) {
        if (i) source += ", ";
        auto callback = std::find_if(
            abi.callbacks.begin(), abi.callbacks.end(),
            [&](const CAbiCallbackText& value) { return value.parameter_index == i; });
        if (callback != abi.callbacks.end())
            source += "beans_ffi_cb" + std::to_string(i);
        else
            source += "*(" + abi.parameter_types[i] + "*)args[" +
                      std::to_string(i) + "]";
    }
    source += ");\n  ";
    for (const CAbiCallbackText& callback : abi.callbacks) {
        std::string prefix = "beans_ffi_cb" +
                             std::to_string(callback.parameter_index);
        source += prefix + "_dispatch = " + prefix +
                  "_old_dispatch;\n  " + prefix + "_context = " + prefix +
                  "_old_context;\n  ";
    }
    if (fn->ret)
        source += "*(" + abi.return_type + "*)result = call_result;\n";
    source += "}\n";

    std::string bridge_error;
    AggregateBridge bridge = load_aggregate_bridge(source, bridge_error);
    if (!bridge) return fail(bridge_error);

    struct AlignedStorage {
        std::vector<std::max_align_t> words;
        explicit AlignedStorage(size_t bytes)
            : words((std::max<size_t>(bytes, 1) + sizeof(std::max_align_t) - 1) /
                    sizeof(std::max_align_t)) {}
        void* data() { return words.data(); }
    };
    std::vector<std::unique_ptr<AlignedStorage>> storage;
    std::vector<void*> pointers;
    std::vector<std::unique_ptr<FfiCallbackContext>> callback_storage;
    std::vector<void*> callback_contexts(fn->params.size(), nullptr);
    for (size_t i = 0; i < fn->params.size(); i++) {
        const TypeRef* parameter = fn->params[i].type.get();
        if (parameter && parameter->kind == TypeRef::Kind::fn) {
            storage.push_back(std::make_unique<AlignedStorage>(sizeof(void*)));
            pointers.push_back(storage.back()->data());
            auto context = std::make_unique<FfiCallbackContext>();
            context->owner = this;
            context->callable = args[i];
            for (const TypePtr& callback_parameter : parameter->fn_params)
                context->parameters.push_back(
                    raw_value_spec(callback_parameter.get()));
            context->returns_value = parameter->fn_ret != nullptr;
            if (context->returns_value)
                context->result = raw_value_spec(parameter->fn_ret.get());
            callback_contexts[i] = context.get();
            callback_storage.push_back(std::move(context));
            continue;
        }
        RawPtrVal spec = raw_value_spec(fn->params[i].type.get());
        storage.push_back(std::make_unique<AlignedStorage>(spec.size));
        spec.address = storage.back()->data();
        raw_write(spec, args[i]);
        pointers.push_back(spec.address);
    }

    if (!fn->ret) {
        bridge(symbol, nullptr, pointers.data(), &Interp::ffi_callback_dispatch,
               callback_contexts.data());
        for (const auto& context : callback_storage)
            if (context->error) std::rethrow_exception(context->error);
        return Value::unit();
    }
    RawPtrVal result_spec = raw_value_spec(fn->ret.get());
    AlignedStorage result(result_spec.size);
    bridge(symbol, result.data(), pointers.data(), &Interp::ffi_callback_dispatch,
           callback_contexts.data());
    for (const auto& context : callback_storage)
        if (context->error) std::rethrow_exception(context->error);
    result_spec.address = result.data();
    return raw_read(result_spec);
}

void Interp::ffi_callback_dispatch(void* raw_context, void* result,
                                   void** arguments) {
    FfiCallbackContext* context = static_cast<FfiCallbackContext*>(raw_context);
    if (!context || context->error) {
        if (context && result && context->result.size)
            std::memset(result, 0, context->result.size);
        return;
    }
    try {
        std::vector<Value> values;
        values.reserve(context->parameters.size());
        for (size_t i = 0; i < context->parameters.size(); i++) {
            RawPtrVal parameter = context->parameters[i];
            parameter.address = arguments[i];
            values.push_back(context->owner->raw_read(parameter));
        }
        Value returned;
        if (context->callable.k == Value::K::closure) {
            returned = context->owner->call_closure(*context->callable.clo,
                                                    std::move(values));
        } else if (context->callable.k == Value::K::fn_ref) {
            const FnDecl* declaration = context->callable.fnr->decl;
            returned = context->owner->call_fn(
                declaration, nullptr, std::move(values),
                pkg_of(declaration->qualname));
        } else {
            throw BeansPanic{"C callback value is not callable", 0, 0};
        }
        if (context->returns_value) {
            RawPtrVal result_spec = context->result;
            result_spec.address = result;
            context->owner->raw_write(result_spec, returned);
        }
    } catch (...) {
        context->error = std::current_exception();
        if (result && context->result.size)
            std::memset(result, 0, context->result.size);
    }
}

Value Interp::call_extern(const FnDecl* fn, const std::vector<Value>& args) {
    auto fail = [&](const std::string& message) -> Value {
        g_beans_panicking = true;
        throw BeansPanic{message, fn->line, fn->col};
    };

    dlerror();
    void* symbol = dlsym(RTLD_DEFAULT, fn->extern_name.c_str());
    const char* lookup_error = dlerror();
    if (!symbol || lookup_error) {
        return fail("C symbol not found: " + fn->extern_name);
    }
    if (args.size() > 6) return fail("extern C calls support at most 6 arguments for now");

    bool has_aggregate = false;
    for (const Param& param : fn->params) {
        if (param.type && param.type->kind == TypeRef::Kind::fn) {
            has_aggregate = true;
            continue;
        }
        RawPtrVal spec = raw_value_spec(param.type.get());
        has_aggregate |= spec.scalar == RawScalar::record ||
                         spec.scalar == RawScalar::array;
    }
    if (fn->ret) {
        RawPtrVal spec = raw_value_spec(fn->ret.get());
        has_aggregate |= spec.scalar == RawScalar::record ||
                         spec.scalar == RawScalar::array;
    }
    if (has_aggregate) return call_extern_aggregate(fn, args, symbol);

    uint64_t words[6] = {};
    for (size_t i = 0; i < args.size(); i++) {
        switch (args[i].k) {
            case Value::K::int_: words[i] = unsigned_integer(args[i]); break;
            case Value::K::bool_: words[i] = args[i].b ? 1 : 0; break;
            case Value::K::rawptr:
                words[i] = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(args[i].raw.address));
                break;
            case Value::K::float_: break;
            default: return fail("unsupported value passed to extern C function");
        }
    }

    bool returns_value = fn->ret != nullptr;
    uint64_t raw = 0;
    bool returns_float = fn->ret &&
                         (fn->ret->name == "f32" || fn->ret->name == "f64" ||
                          fn->ret->name == "float");
    if (returns_float) {
        Value out;
        if (fn->ret->name == "f32") {
            out = Value::of_float(call_c_abi<float>(symbol, args, words));
            out.float_bits = 32;
        } else {
            out = Value::of_float(call_c_abi<double>(symbol, args, words));
        }
        return out;
    }
    if (returns_value) {
        raw = call_c_abi<uint64_t>(symbol, args, words);
    } else {
        call_c_abi<void>(symbol, args, words);
        return Value::unit();
    }

    const TypeRef* ret = fn->ret.get();
    if (ret->name == "bool") return Value::of_bool(raw != 0);
    if (ret->name == "RawPtr") {
        Value out;
        out.k = Value::K::rawptr;
        out.raw = raw_spec(ret);
        out.raw.address = reinterpret_cast<void*>(static_cast<uintptr_t>(raw));
        return out;
    }

    Value out = Value::of_int(signed_integer(raw));
    const std::string& name = ret->name;
    out.int_bits = name == "i8" || name == "u8" || name == "byte" ? 8
                   : name == "i16" || name == "u16"                 ? 16
                   : name == "i32" || name == "u32"                 ? 32
                                                                      : 64;
    out.int_unsigned = name == "u8" || name == "byte" || name == "u16" ||
                       name == "u32" || name == "u64";
    normalize_integer(out);
    return out;
}

Value Interp::call_fn(const FnDecl* fn, Value* self, std::vector<Value> args,
                      const std::string& pkg) {
    if (fn->is_extern_c) return call_extern(fn, args);
    auto env = std::make_shared<Env>();
    if (self) env->declare("self", *self);
    for (size_t i = 0; i < fn->params.size() && i < args.size(); i++) {
        if (fn->params[i].passing == Param::Passing::inout && args[i].inout_ref) {
            env->declare_alias(fn->params[i].name, args[i].inout_ref);
        } else {
            env->declare(fn->params[i].name, std::move(args[i]));
        }
    }

    std::string saved_pkg = g_pkg;
    g_pkg = pkg;
    g_frames.push_back({});
    g_frames.back().ret = fn->ret.get();
    Value result = Value::unit();
    try {
        exec_block(fn->body, env);
    } catch (ReturnSignal& r) {
        result = std::move(r.v);
    } catch (...) {
        // a panic is fatal: defers do not run — matching the native backend,
        // where a panic exits the process without unwinding
        g_frames.pop_back();
        g_pkg = saved_pkg;
        throw;
    }
    auto defers = std::move(g_frames.back().defers);
    g_frames.pop_back();
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
        eval(it->first, it->second); // a panic inside a defer is a real panic
    }
    g_pkg = saved_pkg;
    return result;
}

Value Interp::call_closure(const ClosureVal& c, std::vector<Value> args) {
    const Expr* node = c.node;
    auto env = std::make_shared<Env>();
    env->parent = c.captured;
    for (size_t i = 0; i < node->params.size() && i < args.size(); i++) {
        env->declare(node->params[i].name, std::move(args[i]));
    }

    std::string saved_pkg = g_pkg;
    g_pkg = c.pkg; // the closure runs as code of the package that wrote it
    g_frames.push_back({});
    g_frames.back().ret = node->type.get();
    Value result = Value::unit();
    try {
        exec_block(node->body, env);
    } catch (ReturnSignal& r) {
        result = std::move(r.v);
    } catch (...) {
        g_frames.pop_back();
        g_pkg = saved_pkg;
        throw;
    }
    auto defers = std::move(g_frames.back().defers);
    g_frames.pop_back();
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
        eval(it->first, it->second); // a panic inside a defer is a real panic
    }
    g_pkg = saved_pkg;
    return result;
}

// ---- statements ------------------------------------------------------------

void Interp::exec_block(const std::vector<StmtPtr>& body, std::shared_ptr<Env> env) {
    auto scope = std::make_shared<Env>();
    scope->parent = env;
    for (const StmtPtr& s : body) exec_stmt(s.get(), scope);
}

void Interp::exec_stmt(const Stmt* s, std::shared_ptr<Env>& env) {
    switch (s->kind) {
        case Stmt::Kind::let_: {
            Value v = s->init ? eval(s->init.get(), env, Hint::of(s->type.get()))
                              : Value::unit();
            env->declare(s->name, std::move(v));
            break;
        }
        case Stmt::Kind::assign: {
            if (s->op == TokenKind::assign && s->target &&
                s->target->kind == Expr::Kind::field && s->target->object &&
                s->target->object->kind == Expr::Kind::ident) {
                Value* owner = env->find(std::string(s->target->object->text));
                if (owner && owner->k == Value::K::struct_v && owner->struct_decl &&
                    owner->struct_decl->is_union) {
                    for (const FieldDecl& field : owner->struct_decl->fields) {
                        if (field.name != s->target->name) continue;
                        Value value = eval(s->value.get(), env, Hint::of(field.type.get()));
                        RawPtrVal spec = raw_value_spec(field.type.get());
                        spec.address = owner->union_bytes.data();
                        raw_write(spec, value);
                    }
                    break;
                }
            }
            if (s->op == TokenKind::assign) {
                // hint from the current slot so decimal stays decimal
                Value* slot = lvalue_slot(s->target.get(), env);
                Hint h;
                if (slot && slot->k == Value::K::decimal_) h = Hint::decimal();
                if (slot && slot->k == Value::K::float_) h = Hint::floating();
                Value v = eval(s->value.get(), env, h);
                Value* slot2 = lvalue_slot(s->target.get(), env);
                if (slot2) *slot2 = std::move(v);
                break;
            }
            Value* slot = lvalue_slot(s->target.get(), env);
            if (!slot) break;
            Hint h;
            if (slot->k == Value::K::decimal_) h = Hint::decimal();
            if (slot->k == Value::K::float_) h = Hint::floating();
            Value rhs = eval(s->value.get(), env, h);
            Value* slot2 = lvalue_slot(s->target.get(), env);
            if (!slot2) break;
            switch (slot2->k) {
                case Value::K::int_:
                    {
                    uint64_t lhs = unsigned_integer(*slot2);
                    uint64_t right = unsigned_integer(rhs);
                    switch (s->op) {
                        case TokenKind::plus_eq: lhs += right; break;
                        case TokenKind::minus_eq: lhs -= right; break;
                        case TokenKind::star_eq: lhs *= right; break;
                        case TokenKind::slash_eq:
                            if (right == 0) panic(s->value.get(), "divide by zero");
                            if (slot2->int_unsigned) lhs /= right;
                            else if (!(slot2->i == std::numeric_limits<int64_t>::min() &&
                                       rhs.i == -1))
                                lhs = static_cast<uint64_t>(slot2->i / rhs.i);
                            break;
                        case TokenKind::percent_eq:
                            if (right == 0) panic(s->value.get(), "modulo by zero");
                            if (slot2->int_unsigned) lhs %= right;
                            else if (slot2->i == std::numeric_limits<int64_t>::min() &&
                                     rhs.i == -1)
                                lhs = 0;
                            else
                                lhs = static_cast<uint64_t>(slot2->i % rhs.i);
                            break;
                        default: break;
                    }
                    slot2->i = signed_integer(lhs);
                    normalize_integer(*slot2);
                    }
                    break;
                case Value::K::float_:
                    switch (s->op) {
                        case TokenKind::plus_eq: slot2->f += rhs.f; break;
                        case TokenKind::minus_eq: slot2->f -= rhs.f; break;
                        case TokenKind::star_eq: slot2->f *= rhs.f; break;
                        case TokenKind::slash_eq: slot2->f /= rhs.f; break;
                        default: break;
                    }
                    normalize_float(*slot2);
                    break;
                case Value::K::decimal_:
                    switch (s->op) {
                        case TokenKind::plus_eq: slot2->dec = slot2->dec.add(rhs.dec); break;
                        case TokenKind::minus_eq: slot2->dec = slot2->dec.sub(rhs.dec); break;
                        case TokenKind::star_eq: slot2->dec = slot2->dec.mul(rhs.dec); break;
                        case TokenKind::slash_eq:
                            if (rhs.dec.coeff == 0) panic(s->value.get(), "divide by zero");
                            slot2->dec = slot2->dec.div(rhs.dec);
                            break;
                        default: break;
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        case Stmt::Kind::expr:
            eval(s->expr.get(), env);
            break;
        case Stmt::Kind::ret: {
            ReturnSignal r;
            const TypeRef* rt = g_frames.empty() ? nullptr : g_frames.back().ret;
            r.v = s->expr ? eval(s->expr.get(), env, Hint::of(rt)) : Value::unit();
            throw r;
        }
        case Stmt::Kind::brk:
            throw BreakSignal{};
        case Stmt::Kind::cont:
            throw ContinueSignal{};
        case Stmt::Kind::if_: {
            Value c = eval(s->cond.get(), env);
            if (c.b) exec_block(s->body, env);
            else if (!s->else_body.empty()) exec_block(s->else_body, env);
            break;
        }
        case Stmt::Kind::for_ever: {
            while (true) {
                try {
                    exec_block(s->body, env);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case Stmt::Kind::for_while: {
            while (true) {
                Value c = eval(s->cond.get(), env);
                if (!c.b) break;
                try {
                    exec_block(s->body, env);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case Stmt::Kind::for_in: {
            Value it = eval(s->iterable.get(), env);
            auto run_iter = [&](Value item) -> bool {
                auto iter_env = std::make_shared<Env>();
                iter_env->parent = env;
                iter_env->declare(s->loop_var, std::move(item));
                try {
                    exec_block(s->body, iter_env);
                } catch (BreakSignal&) {
                    return false;
                } catch (ContinueSignal&) {
                }
                return true;
            };
            if (it.k == Value::K::range) {
                Value item = Value::of_int(it.range->lo);
                item.int_bits = it.range->bits;
                item.int_unsigned = it.range->is_unsigned;
                while (true) {
                    uint64_t end_bits;
                    std::memcpy(&end_bits, &it.range->hi, sizeof end_bits);
                    bool before = item.int_unsigned ? unsigned_integer(item) < end_bits
                                                    : item.i < it.range->hi;
                    bool equal = item.i == it.range->hi;
                    if (!before && !(it.range->inclusive && equal)) break;
                    if (!run_iter(item) || (it.range->inclusive && equal)) break;
                    item.i = signed_integer(unsigned_integer(item) + 1);
                    normalize_integer(item);
                }
            } else if (it.k == Value::K::list) {
                for (size_t idx = 0; idx < it.list->items.size(); idx++) {
                    if (!run_iter(it.list->items[idx])) break;
                }
            } else if (it.k == Value::K::fixed_array) {
                for (const Value& item : it.array) {
                    if (!run_iter(item)) break;
                }
            } else if (it.k == Value::K::slice) {
                for (int64_t index = 0; index < it.slice_len; index++) {
                    RawPtrVal element = it.slice_ptr;
                    element.address = static_cast<void*>(
                        static_cast<char*>(element.address) + index * element.size);
                    if (!run_iter(raw_read(element))) break;
                }
            }
            break;
        }
        case Stmt::Kind::defer_:
            if (!g_frames.empty()) {
                g_frames.back().defers.emplace_back(s->expr.get(), env);
            }
            break;
        case Stmt::Kind::unsafe_:
            exec_block(s->body, env);
            break;
    }
}

// ---- lvalues ---------------------------------------------------------------

Value* Interp::lvalue_slot(const Expr* target, std::shared_ptr<Env>& env) {
    if (target->kind == Expr::Kind::ident) {
        return env->find(std::string(target->text));
    }
    if (target->kind == Expr::Kind::field) {
        if (target->object->kind == Expr::Kind::ident) {
            Value* owner = env->find(std::string(target->object->text));
            if (owner && owner->k == Value::K::struct_v && owner->struct_decl) {
                for (size_t i = 0; i < owner->struct_decl->fields.size(); i++) {
                    if (owner->struct_decl->fields[i].name == target->name)
                        return &owner->struct_fields[i];
                }
            }
        }
        Value obj = eval(target->object.get(), env);
        if (obj.k == Value::K::instance) {
            // instance is shared — the slot stays valid after obj dies
            return obj.inst->field(target->name);
        }
        panic(target, "can't assign to this field");
    }
    if (target->kind == Expr::Kind::index) {
        if (target->object->kind == Expr::Kind::ident) {
            Value* owner = env->find(std::string(target->object->text));
            if (owner && owner->k == Value::K::fixed_array) {
                Value idx = eval(target->index_expr.get(), env);
                if (idx.i < 0 || static_cast<size_t>(idx.i) >= owner->array.size()) {
                    panic(target, "array index " + std::to_string(idx.i) +
                                      " out of range (len " +
                                      std::to_string(owner->array.size()) + ")");
                }
                return &owner->array[static_cast<size_t>(idx.i)];
            }
        }
        Value obj = eval(target->object.get(), env);
        Value idx = eval(target->index_expr.get(), env,
                         obj.k == Value::K::map ? map_key_hint(*obj.map) : Hint{});
        if (obj.k == Value::K::list) {
            int64_t i = idx.i;
            if (i < 0 || static_cast<size_t>(i) >= obj.list->items.size()) {
                panic(target, "list index " + std::to_string(i) + " out of range");
            }
            return &obj.list->items[static_cast<size_t>(i)];
        }
        if (obj.k == Value::K::map) {
            uint64_t h = 0;
            size_t i = map_find(*obj.map, idx, h);
            if (i != SIZE_MAX) return &obj.map->entries[i].second;
            map_append(*obj.map, h, idx, Value::unit());
            return &obj.map->entries.back().second;
        }
    }
    panic(target, "can't assign here");
}

void Interp::assign_to(const Expr* target, Value v, std::shared_ptr<Env>& env) {
    if (Value* slot = lvalue_slot(target, env)) *slot = std::move(v);
}

// ---- expressions -----------------------------------------------------------

Value Interp::eval(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    switch (e->kind) {
        case Expr::Kind::int_lit: {
            TypeId checked = hir_.type_of(e);
            bool checked_dec = checked && checked->k == Type::K::decimal_;
            bool checked_float = checked && checked->is_float();
            // HIR wins. Runtime hints only decide literals in interpolation
            // segments, which are parsed after checking and have no HIR row.
            if (checked_dec || (!checked &&
                                (e->numk == 3 || hint.wants_dec())))
                return Value::of_dec(Decimal::parse(clean_number(e->text)));
            if (checked_float || (!checked &&
                                  (e->numk == 2 || hint.wants_float())))
                return typed_float(std::strtod(clean_number(e->text).c_str(), nullptr),
                                   checked, hint.tref);
            return typed_integer(parse_int_text(e->text), checked, hint.tref);
        }
        case Expr::Kind::float_lit: {
            TypeId checked = hir_.type_of(e);
            bool checked_dec = checked && checked->k == Type::K::decimal_;
            if (checked_dec || (!checked &&
                                (e->numk == 3 || hint.wants_dec())))
                return Value::of_dec(Decimal::parse(clean_number(e->text)));
            return typed_float(std::strtod(clean_number(e->text).c_str(), nullptr),
                               checked, hint.tref);
        }
        case Expr::Kind::string_lit:
            return eval_string(e, env);
        case Expr::Kind::bool_lit:
            return Value::of_bool(e->bool_val);
        case Expr::Kind::ident: {
            std::string name(e->text);
            if (Value* v = env->find(name)) return *v;
            if (name == "none") return none();
            auto fit = fns_.find(e->resolved.empty() ? qual(name) : e->resolved);
            if (fit != fns_.end()) {
                Value v;
                v.k = Value::K::fn_ref;
                v.fnr = std::make_shared<FnRefVal>();
                v.fnr->decl = fit->second;
                return v;
            }
            panic(e, "unknown name '" + name + "'");
        }
        case Expr::Kind::self_ref: {
            if (Value* v = env->find("self")) return *v;
            panic(e, "self not bound");
        }
        case Expr::Kind::unary: {
            if (e->op == TokenKind::kw_inout) {
                Value out = Value::unit();
                out.inout_ref = env->find(std::string(e->rhs->text));
                return out;
            }
            if (e->op == TokenKind::kw_move) {
                Value* slot = env->find(std::string(e->rhs->text));
                Value moved = std::move(*slot);
                *slot = Value::unit();
                return moved;
            }
            if (e->op == TokenKind::minus) {
                    Value v = eval(e->rhs.get(), env, hint);
                    switch (v.k) {
                    case Value::K::int_: {
                        uint64_t bits = 0 - unsigned_integer(v);
                        v.i = signed_integer(bits);
                        normalize_integer(v);
                        return v;
                    }
                    case Value::K::float_: v.f = -v.f; normalize_float(v); return v;
                    case Value::K::decimal_: v.dec = v.dec.neg(); return v;
                    default: return v;
                }
            }
            if (e->op == TokenKind::bang) {
                Value v = eval(e->rhs.get(), env);
                return Value::of_bool(!v.b);
            }
            Value v = eval(e->rhs.get(), env); // ~
            v.i = ~v.i;
            normalize_integer(v);
            return v;
        }
        case Expr::Kind::binary:
            return eval_binary(e, env);
        case Expr::Kind::range: {
            Value lo = eval(e->lhs.get(), env);
            Value hi = eval(e->rhs.get(), env);
            Value v;
            v.k = Value::K::range;
            v.range = std::make_shared<RangeVal>();
            v.range->lo = lo.i;
            v.range->hi = hi.i;
            v.range->bits = lo.int_bits;
            v.range->is_unsigned = lo.int_unsigned;
            v.range->inclusive = e->inclusive;
            return v;
        }
        case Expr::Kind::new_:
            return eval_new(e, env, hint);
        case Expr::Kind::call:
            return eval_call(e, env, hint);
        case Expr::Kind::field: {
            const Expr* obj = e->object.get();
            if (obj->kind == Expr::Kind::ident && !env->find(std::string(obj->text))) {
                std::string n(obj->text);
                if (const EnumDecl* ed = resolve_enum(obj, n)) {
                    Value v;
                    v.k = Value::K::enum_v;
                    v.en = std::make_shared<EnumVal>();
                    v.en->enum_name = ed->qualname;
                    v.en->variant = e->name;
                    return v;
                }
            }
            if (obj->kind == Expr::Kind::field && !obj->resolved.empty()) {
                // `util.Status.active` — the checker pinned the enum
                if (const EnumDecl* ed = resolve_enum(obj, "")) {
                    Value v;
                    v.k = Value::K::enum_v;
                    v.en = std::make_shared<EnumVal>();
                    v.en->enum_name = ed->qualname;
                    v.en->variant = e->name;
                    return v;
                }
            }
            Value v = eval(obj, env);
            if (v.k == Value::K::instance) {
                if (Value* f = v.inst->field(e->name)) return *f;
                panic(e, "no field '" + e->name + "'");
            }
            if (v.k == Value::K::struct_v && v.struct_decl) {
                for (size_t i = 0; i < v.struct_decl->fields.size(); i++) {
                    if (v.struct_decl->fields[i].name != e->name) continue;
                    if (v.struct_decl->is_union) {
                        RawPtrVal spec = raw_value_spec(v.struct_decl->fields[i].type.get());
                        spec.address = v.union_bytes.data();
                        return raw_read(spec);
                    }
                    if (i < v.struct_fields.size())
                        return v.struct_fields[i];
                }
                panic(e, "no field '" + e->name + "'");
            }
            panic(e, "no field '" + e->name + "' on this value");
        }
        case Expr::Kind::index: {
            Value obj = eval(e->object.get(), env);
            Value idx = eval(e->index_expr.get(), env,
                             obj.k == Value::K::map ? map_key_hint(*obj.map) : Hint{});
            if (obj.k == Value::K::list) {
                int64_t i = idx.i;
                if (i < 0 || static_cast<size_t>(i) >= obj.list->items.size()) {
                    panic(e, "list index " + std::to_string(i) + " out of range (len " +
                                 std::to_string(obj.list->items.size()) + ")");
                }
                return obj.list->items[static_cast<size_t>(i)];
            }
            if (obj.k == Value::K::slice) {
                int64_t i = idx.i;
                if (i < 0 || i >= obj.slice_len) {
                    panic(e, "slice index " + std::to_string(i) +
                                 " out of range (len " +
                                 std::to_string(obj.slice_len) + ")");
                }
                RawPtrVal element = obj.slice_ptr;
                element.address = static_cast<void*>(
                    static_cast<char*>(element.address) + i * element.size);
                return raw_read(element);
            }
            if (obj.k == Value::K::fixed_array) {
                int64_t i = idx.i;
                if (i < 0 || static_cast<size_t>(i) >= obj.array.size()) {
                    panic(e, "array index " + std::to_string(i) +
                                 " out of range (len " +
                                 std::to_string(obj.array.size()) + ")");
                }
                return obj.array[static_cast<size_t>(i)];
            }
            if (obj.k == Value::K::map) {
                uint64_t h = 0;
                size_t i = map_find(*obj.map, idx, h);
                if (i != SIZE_MAX) return obj.map->entries[i].second;
                panic(e, "map key not found: " + display(idx));
            }
            panic(e, "can't index this value");
        }
        case Expr::Kind::list_lit: {
            Value v;
            TypeId checked = hir_.type_of(e);
            if (hint.fixed_array ||
                (hint.tref && hint.tref->kind == TypeRef::Kind::fixed_array) ||
                (checked && checked->k == Type::K::fixed_array)) {
                v.k = Value::K::fixed_array;
                Hint elem = Hint::of(hint.arg(0));
                for (const ExprPtr& el : e->args)
                    v.array.push_back(eval(el.get(), env, elem));
                return v;
            }
            v.k = Value::K::list;
            v.list = std::make_shared<ListVal>();
            Hint elem = Hint::of(hint.arg(0));
            for (const ExprPtr& el : e->args) {
                v.list->items.push_back(eval(el.get(), env, elem));
            }
            return v;
        }
        case Expr::Kind::init:
            return eval_init(e, env, hint);
        case Expr::Kind::cast: {
            Value v = eval(e->object.get(), env);
            const TypeRef* t = e->type.get();
            std::string tn = t ? (t->resolved.empty() ? t->name : t->resolved) : "";
            if (e->checked) {
                // as? targets are always user classes — fall back to the
                // running package's name for unchecked ASTs
                if (t && t->resolved.empty()) tn = qual(t->name);
                if (v.k == Value::K::instance && class_is(v.inst->cls, tn)) return some(v);
                return none();
            }
            if (tn == "decimal") {
                if (v.k == Value::K::int_) return Value::of_dec(Decimal::from_int(v.i));
                if (v.k == Value::K::float_) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%.17g", v.f);
                    return Value::of_dec(Decimal::parse(buf));
                }
                return v;
            }
            if (tn == "float" || tn == "f64" || tn == "f32") {
                TypeId to = hir_.type_of(e);
                if (v.k == Value::K::int_) {
                    double number = v.int_unsigned
                                        ? static_cast<double>(unsigned_integer(v))
                                        : static_cast<double>(v.i);
                    return typed_float(number, to, e->type.get());
                }
                if (v.k == Value::K::decimal_)
                    return typed_float(v.dec.to_double(), to, e->type.get());
                v.float_bits = tn == "f32" ? 32 : 64;
                normalize_float(v);
                return v;
            }
            if (tn == "int" || tn == "i64" || tn == "i8" || tn == "i16" ||
                tn == "i32" || tn == "u8" || tn == "byte" || tn == "u16" ||
                tn == "u32" || tn == "u64") {
                int64_t bits = v.i;
                if (v.k == Value::K::float_) bits = static_cast<int64_t>(v.f);
                if (v.k == Value::K::decimal_) bits = v.dec.to_int();
                return typed_integer(bits, hir_.type_of(e), e->type.get());
            }
            return v; // upcasts are free
        }
        case Expr::Kind::try_: {
            Value v = eval(e->object.get(), env);
            if (v.k == Value::K::enum_v) {
                if (v.en->variant == "ok" || v.en->variant == "some") {
                    return v.en->payload.empty() ? Value::unit() : v.en->payload[0];
                }
                ReturnSignal r;
                r.v = v;
                throw r;
            }
            panic(e, "? on a non-Result value");
        }
        case Expr::Kind::closure: {
            Value v;
            v.k = Value::K::closure;
            v.clo = std::make_shared<ClosureVal>();
            v.clo->node = e;
            v.clo->captured = env;
            v.clo->pkg = g_pkg;
            return v;
        }
        case Expr::Kind::if_expr: {
            Value c = eval(e->cond.get(), env);
            return c.b ? eval(e->then_e.get(), env, hint)
                       : eval(e->else_e.get(), env, hint);
        }
        case Expr::Kind::match_expr:
            return eval_match(e, env, hint);
    }
    return Value::unit();
}

Value Interp::eval_binary(const Expr* e, std::shared_ptr<Env>& env) {
    // short-circuit first
    if (e->op == TokenKind::andand) {
        Value l = eval(e->lhs.get(), env);
        if (!l.b) return Value::of_bool(false);
        return Value::of_bool(eval(e->rhs.get(), env).b);
    }
    if (e->op == TokenKind::oror) {
        Value l = eval(e->lhs.get(), env);
        if (l.b) return Value::of_bool(true);
        return Value::of_bool(eval(e->rhs.get(), env).b);
    }

    Value l = eval(e->lhs.get(), env);
    Hint rh;
    if (l.k == Value::K::decimal_) rh = Hint::decimal();
    if (l.k == Value::K::float_) rh = Hint::floating();
    Value r = eval(e->rhs.get(), env, rh);
    if (l.k != r.k) {
        // literal on the left adapting to the right (checker guaranteed validity)
        Hint lh;
        if (r.k == Value::K::decimal_) lh = Hint::decimal();
        if (r.k == Value::K::float_) lh = Hint::floating();
        l = eval(e->lhs.get(), env, lh);
    }

    TokenKind op = e->op;
    auto cmp_result = [&](int c) -> Value {
        switch (op) {
            case TokenKind::eq: return Value::of_bool(c == 0);
            case TokenKind::neq: return Value::of_bool(c != 0);
            case TokenKind::lt: return Value::of_bool(c < 0);
            case TokenKind::le: return Value::of_bool(c <= 0);
            case TokenKind::gt: return Value::of_bool(c > 0);
            case TokenKind::ge: return Value::of_bool(c >= 0);
            default: return Value::of_bool(false);
        }
    };

    switch (l.k) {
        case Value::K::int_: {
            int64_t a = l.i, b = r.i;
            uint64_t ua = unsigned_integer(l), ub = unsigned_integer(r);
            auto result = [&](uint64_t bits) {
                Value value = l;
                value.i = signed_integer(bits);
                normalize_integer(value);
                return value;
            };
            auto compare = [&]() {
                if (l.int_unsigned)
                    return cmp_result(ua < ub ? -1 : ua > ub ? 1 : 0);
                return cmp_result(a < b ? -1 : a > b ? 1 : 0);
            };
            switch (op) {
                case TokenKind::plus: return result(ua + ub);
                case TokenKind::minus: return result(ua - ub);
                case TokenKind::star: return result(ua * ub);
                case TokenKind::slash:
                    if (ub == 0) panic(e, "divide by zero");
                    if (l.int_unsigned) return result(ua / ub);
                    if (b == -1 && a == std::numeric_limits<int64_t>::min())
                        return result(ua);
                    return result(static_cast<uint64_t>(a / b));
                case TokenKind::percent:
                    if (ub == 0) panic(e, "modulo by zero");
                    if (l.int_unsigned) return result(ua % ub);
                    if (b == -1 && a == std::numeric_limits<int64_t>::min()) return result(0);
                    return result(static_cast<uint64_t>(a % b));
                case TokenKind::shl:
                    return result(ua << (ub & ((l.int_bits ? l.int_bits : 64) - 1)));
                case TokenKind::shr:
                    return l.int_unsigned
                               ? result(ua >> (ub & ((l.int_bits ? l.int_bits : 64) - 1)))
                               : result(static_cast<uint64_t>(
                                     a >> (ub & ((l.int_bits ? l.int_bits : 64) - 1))));
                case TokenKind::amp: return result(ua & ub);
                case TokenKind::pipe: return result(ua | ub);
                case TokenKind::caret: return result(ua ^ ub);
                default: return compare();
            }
        }
        case Value::K::float_: {
            double a = l.f, b = r.f;
            auto result = [&](double number) {
                Value value = l;
                value.f = number;
                normalize_float(value);
                return value;
            };
            switch (op) {
                case TokenKind::plus: return result(a + b);
                case TokenKind::minus: return result(a - b);
                case TokenKind::star: return result(a * b);
                case TokenKind::slash: return result(a / b);
                default: return cmp_result(a < b ? -1 : a > b ? 1 : 0);
            }
        }
        case Value::K::simd4f32: {
            Value out;
            out.k = Value::K::simd4f32;
            for (size_t lane = 0; lane < out.simd.size(); lane++) {
                switch (op) {
                    case TokenKind::plus: out.simd[lane] = l.simd[lane] + r.simd[lane]; break;
                    case TokenKind::minus: out.simd[lane] = l.simd[lane] - r.simd[lane]; break;
                    case TokenKind::star: out.simd[lane] = l.simd[lane] * r.simd[lane]; break;
                    case TokenKind::slash: out.simd[lane] = l.simd[lane] / r.simd[lane]; break;
                    default: break;
                }
            }
            return out;
        }
        case Value::K::decimal_: {
            switch (op) {
                case TokenKind::plus: return Value::of_dec(l.dec.add(r.dec));
                case TokenKind::minus: return Value::of_dec(l.dec.sub(r.dec));
                case TokenKind::star: return Value::of_dec(l.dec.mul(r.dec));
                case TokenKind::slash:
                    if (r.dec.coeff == 0) panic(e, "divide by zero");
                    return Value::of_dec(l.dec.div(r.dec));
                default: return cmp_result(l.dec.cmp(r.dec));
            }
        }
        case Value::K::string_: {
            int c = l.s->compare(*r.s);
            return cmp_result(c < 0 ? -1 : c > 0 ? 1 : 0);
        }
        case Value::K::bool_: {
            switch (op) {
                case TokenKind::eq: return Value::of_bool(l.b == r.b);
                case TokenKind::neq: return Value::of_bool(l.b != r.b);
                case TokenKind::lt: return Value::of_bool(!l.b && r.b);
                case TokenKind::le: return Value::of_bool(l.b == r.b || (!l.b && r.b));
                case TokenKind::gt: return Value::of_bool(l.b && !r.b);
                case TokenKind::ge: return Value::of_bool(l.b == r.b || (l.b && !r.b));
                default: break;
            }
            return Value::of_bool(false);
        }
        case Value::K::enum_v:
        case Value::K::bytes:
        case Value::K::rawptr:
        case Value::K::fixed_array: {
            bool same = value_eq(l, r);
            return Value::of_bool(op == TokenKind::eq ? same : !same);
        }
        case Value::K::struct_v: {
            bool same = value_eq(l, r);
            return Value::of_bool(op == TokenKind::eq ? same : !same);
        }
        default:
            return Value::of_bool(false);
    }
}

Value Interp::eval_init(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    std::string cname = e->resolved.empty() ? e->name : e->resolved;
    bool pinned = !e->resolved.empty();
    if (cname.empty() && hint.tref && hint.tref->kind == TypeRef::Kind::named) {
        if (hint.tref->name == "Map" || hint.tref->name == "OrderedMap") {
            Value v;
            v.k = Value::K::map;
            v.map = std::make_shared<MapVal>();
            v.map->ordered = hint.tref->name == "OrderedMap";
            Hint kh = Hint::of(hint.arg(0));
            Hint vh = Hint::of(hint.arg(1));
            for (const InitEntry& en : e->entries) {
                Value key = en.key ? eval(en.key.get(), env, kh)
                                   : Value::of_str(en.name);
                Value val = eval(en.value.get(), env, vh);
                // set, not push: a duplicate literal key overwrites, like native
                map_set(*v.map, std::move(key), std::move(val));
            }
            return v;
        }
        if (!hint.tref->resolved.empty()) {
            cname = hint.tref->resolved;
            pinned = true;
        } else {
            cname = hint.tref->name;
        }
    }
    if (const ClassDecl* cls = find_class(cname)) {
        return make_instance(cls, e->entries, env);
    }
    if (!pinned && !cname.empty()) {
        // plain name in a dep package's own code (interpolation segments)
        if (const ClassDecl* cls = find_class(qual(cname))) {
            return make_instance(cls, e->entries, env);
        }
    }
    // map literal with string keys, no hint needed at runtime
    if (cname.empty() || cname == "Map" || cname == "OrderedMap") {
        Value v;
        v.k = Value::K::map;
        v.map = std::make_shared<MapVal>();
        v.map->ordered = cname == "OrderedMap";
        for (const InitEntry& en : e->entries) {
            Value key = en.key ? eval(en.key.get(), env) : Value::of_str(en.name);
            map_set(*v.map, std::move(key), eval(en.value.get(), env));
        }
        return v;
    }
    panic(e, "can't build '" + cname + "'");
}

// ---- calls ------------------------------------------------------------------

Value Interp::eval_call(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    const Expr* callee = e->callee.get();

    auto eval_args_hinted = [&](const std::vector<Param>& params) {
        std::vector<Value> out;
        for (size_t i = 0; i < e->args.size(); i++) {
            Hint h = i < params.size() ? Hint::of(params[i].type.get()) : Hint();
            out.push_back(eval(e->args[i].get(), env, h));
        }
        return out;
    };
    auto eval_args_plain = [&]() {
        std::vector<Value> out;
        for (const ExprPtr& a : e->args) out.push_back(eval(a.get(), env));
        return out;
    };
    auto call_value = [&](Value& f, std::vector<Value> args) -> Value {
        if (f.k == Value::K::closure) return call_closure(*f.clo, std::move(args));
        if (f.k == Value::K::fn_ref)
            return call_fn(f.fnr->decl, nullptr, std::move(args),
                           pkg_of(f.fnr->decl->qualname));
        panic(e, "not callable");
    };

    if (callee->kind == Expr::Kind::ident) {
        std::string name(callee->text);

        if (Value* v = env->find(name)) {
            Value fv = *v; // copy before arg eval — env may grow underneath
            std::vector<Value> args;
            if (fv.k == Value::K::closure) {
                for (size_t i = 0; i < e->args.size(); i++) {
                    Hint h = i < fv.clo->node->params.size()
                                 ? Hint::of(fv.clo->node->params[i].type.get())
                                 : Hint();
                    args.push_back(eval(e->args[i].get(), env, h));
                }
            } else {
                args = eval_args_plain();
            }
            return call_value(fv, std::move(args));
        }

        if (name == "some") {
            return some(eval(e->args[0].get(), env, Hint::of(hint.arg(0))));
        }
        if (name == "ok" || name == "err") {
            Value x;
            x.k = Value::K::enum_v;
            x.en = std::make_shared<EnumVal>();
            x.en->enum_name = "Result";
            x.en->variant = name;
            if (name == "ok") {
                x.en->payload.push_back(eval(e->args[0].get(), env, Hint::of(hint.arg(0))));
            } else {
                const TypeRef* error_hint = hint.arg(1);
                Value arg = eval(e->args[0].get(), env, Hint::of(error_hint));
                bool default_error = !error_hint ||
                                     (error_hint->kind == TypeRef::Kind::named &&
                                      error_hint->name == "Error");
                if (default_error && arg.k == Value::K::string_)
                    arg = make_err(*arg.s);
                x.en->payload.push_back(std::move(arg));
            }
            return x;
        }

        auto fit = fns_.find(callee->resolved.empty() ? qual(name) : callee->resolved);
        if (fit != fns_.end()) {
            return call_fn(fit->second, nullptr, eval_args_hinted(fit->second->params),
                           pkg_of(fit->second->qualname));
        }

        // a class name called like a function: construction through init.
        // Resolution mirrors eval_init — checker key first, then the plain
        // name, then the current package (re-parsed interpolation segments).
        if (!callee->resolved.empty()) {
            if (const ClassDecl* cls = find_class(callee->resolved)) {
                return construct(cls, e, env);
            }
        } else {
            if (const ClassDecl* cls = find_class(name)) return construct(cls, e, env);
            if (const ClassDecl* cls = find_class(qual(name))) return construct(cls, e, env);
        }
        panic(e, "unknown function '" + name + "'");
    }

    if (callee->kind == Expr::Kind::field) {
        const Expr* obj = callee->object.get();
        const std::string& mname = callee->name;

        auto do_println = [&](bool newline, bool err = false) {
            Value v = eval(e->args[0].get(), env);
            std::string out = display(v);
            if (newline) out += "\n";
            std::fwrite(out.data(), 1, out.size(), err ? stderr : stdout);
            return Value::unit();
        };
        auto do_builtin_fn = [&](const BuiltinFn& b) {
            std::vector<Value> args;
            for (size_t i = 0; i < e->args.size(); i++) {
                args.push_back(eval(e->args[i].get(), env,
                                    i < b.params.size() ? hint_of_bt(b.params[i]) : Hint()));
            }
            return b.run(e->line, e->col, args);
        };
        auto do_spawn = [&]() {
            Value f = eval(e->args[0].get(), env);
            Value tv;
            tv.k = Value::K::thread;
            tv.thread = std::make_shared<ThreadVal>();
            tv.thread->result = std::make_shared<Value>();
            tv.thread->panic = std::make_shared<std::string>();
            auto result = tv.thread->result;
            auto panic_slot = tv.thread->panic;
            ClosureVal clo = *f.clo;
            beans_threads_inc(); // a File closed on another thread now defers
                                 // its real fd close until its handle drops
            tv.thread->th = std::thread([this, clo, result, panic_slot]() {
                g_interp_tl = this; // deinit can fire on this thread too
                try {
                    *result = call_closure(clo, {});
                } catch (const BeansPanic& p) {
                    *panic_slot = p.msg.empty() ? "panic" : p.msg;
                    g_beans_panicking = false; // stored, this thread lives on
                } catch (...) {
                    *panic_slot = "unknown panic";
                    g_beans_panicking = false;
                }
                beans_threads_dec();
            });
            return tv;
        };

        // the checker pinned this call: a std builtin or a package function
        if (!callee->resolved.empty()) {
            const std::string& r = callee->resolved;
            if (r == "std.io.println") return do_println(true);
            if (r == "std.io.print") return do_println(false);
            if (r == "std.io.eprintln") return do_println(true, true);
            if (r == "std.io.eprint") return do_println(false, true);
            if (r == "std.thread.spawn") return do_spawn();
            for (const BuiltinFn& b : builtin_fns()) {
                if (r == std::string(b.module) + "." + b.name) return do_builtin_fn(b);
            }
            auto fit = fns_.find(r);
            if (fit != fns_.end()) {
                return call_fn(fit->second, nullptr, eval_args_hinted(fit->second->params),
                               pkg_of(fit->second->qualname));
            }
            // super.init(...): the checker resolved the ancestor class — run
            // its init on the live self, construction is not restarted
            if (mname == "init" && obj->kind == Expr::Kind::ident &&
                obj->text == "super") {
                const ClassDecl* anc = find_class(r);
                const FnDecl* ini = nullptr;
                if (anc) {
                    for (const FnDecl& m : anc->methods) {
                        if (m.has_self && m.name == "init") ini = &m;
                    }
                }
                Value* self = env->find("self");
                if (!ini || !self) panic(e, "super.init here");
                std::vector<Value> args;
                for (size_t i = 0; i < e->args.size(); i++) {
                    Hint h = i < ini->params.size() ? Hint::of(ini->params[i].type.get())
                                                    : Hint();
                    args.push_back(eval(e->args[i].get(), env, h));
                }
                Value sv = *self;
                return call_fn(ini, &sv, std::move(args), pkg_of(anc->qualname));
            }
            // pkg.Class(args) — the checker pinned the class key
            if (const ClassDecl* cls = find_class(r)) return construct(cls, e, env);
        }

        if (obj->kind == Expr::Kind::ident && !env->find(std::string(obj->text))) {
            std::string n(obj->text);

            // unannotated package call (string-interpolation segments)
            std::string path = binding_path(n);
            if (!path.empty()) {
                if (path == "std.io" && (mname == "println" || mname == "print" ||
                                         mname == "eprintln" || mname == "eprint")) {
                    return do_println(mname == "println" || mname == "eprintln",
                                      mname == "eprintln" || mname == "eprint");
                }
                for (const BuiltinFn& b : builtin_fns()) {
                    if (path == b.module && mname == b.name) return do_builtin_fn(b);
                }
                if (path == "std.thread" && mname == "spawn") return do_spawn();
                auto pfx = prefix_by_path_.find(path);
                if (pfx != prefix_by_path_.end()) {
                    auto fit = fns_.find(pfx->second + "." + mname);
                    if (fit != fns_.end()) {
                        return call_fn(fit->second, nullptr,
                                       eval_args_hinted(fit->second->params),
                                       pkg_of(fit->second->qualname));
                    }
                }
                panic(e, "package '" + n + "' has nothing runnable named '" + mname + "'");
            }

            if (const EnumDecl* ed = resolve_enum(obj, n)) {
                Value x;
                x.k = Value::K::enum_v;
                x.en = std::make_shared<EnumVal>();
                x.en->enum_name = ed->qualname;
                x.en->variant = mname;
                const EnumVariant* var = nullptr;
                for (const EnumVariant& v : ed->variants) {
                    if (v.name == mname) var = &v;
                }
                for (size_t i = 0; i < e->args.size(); i++) {
                    Hint h = var && i < var->payload.size()
                                 ? Hint::of(var->payload[i].type.get())
                                 : Hint();
                    x.en->payload.push_back(eval(e->args[i].get(), env, h));
                }
                return x;
            }

            if (n == "Slice" && mname == "from_raw") {
                Value pointer = eval(e->args[0].get(), env);
                Value length = eval(e->args[1].get(), env);
                if (length.i < 0) panic(e, "negative slice length");
                if (length.i > 0 && !pointer.raw.address)
                    panic(e, "null pointer with non-empty slice");
                Value value;
                value.k = Value::K::slice;
                value.slice_ptr = pointer.raw;
                value.slice_len = length.i;
                return value;
            }

            if (n == "Simd4f32") {
                Value value;
                value.k = Value::K::simd4f32;
                if (mname == "splat") {
                    Value scalar = eval(e->args[0].get(), env, Hint::floating());
                    value.simd.fill(static_cast<float>(scalar.f));
                    return value;
                }
                if (mname == "of") {
                    for (size_t i = 0; i < value.simd.size(); i++) {
                        Value scalar = eval(e->args[i].get(), env, Hint::floating());
                        value.simd[i] = static_cast<float>(scalar.f);
                    }
                    return value;
                }
                if (mname == "load") {
                    Value pointer = eval(e->args[0].get(), env);
                    if (!pointer.raw.address) panic(e, "null SIMD load");
                    std::memcpy(value.simd.data(), pointer.raw.address,
                                sizeof(float) * value.simd.size());
                    return value;
                }
            }

            if (n == "RawPtr") {
                Value value;
                value.k = Value::K::rawptr;
                value.raw = raw_spec(hir_.type_of(e));
                if (mname == "alloc") {
                    Value count = eval(e->args[0].get(), env);
                    if (count.i < 0) panic(e, "negative raw allocation count");
                    uint64_t amount = static_cast<uint64_t>(count.i);
                    if (!value.raw.size || amount > (uint64_t{1} << 58) / value.raw.size)
                        panic(e, "raw allocation too large");
                    value.raw.address = std::calloc(static_cast<size_t>(amount),
                                                    value.raw.size);
                    if (!value.raw.address && amount) panic(e, "out of memory");
                    return value;
                }
                if (mname == "from_address") {
                    Value address = eval(e->args[0].get(), env);
                    value.raw.address = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(unsigned_integer(address)));
                    return value;
                }
                if (mname == "null") return value;
            }

            if (n == "Bytes" || n == "File" || n == "Dir" || n == "MMap") {
                for (const BuiltinStatic& b : builtin_statics()) {
                    if (n == b.cls && mname == b.name) {
                        std::vector<Value> args;
                        for (size_t i = 0; i < e->args.size(); i++) {
                            args.push_back(
                                eval(e->args[i].get(), env,
                                     i < b.params.size() ? hint_of_bt(b.params[i]) : Hint()));
                        }
                        return b.run(e->line, e->col, args);
                    }
                }
            }

            if (const ClassDecl* cls = resolve_class(obj, n)) {
                const FnDecl* m = nullptr;
                for (const FnDecl& f : cls->methods) {
                    if (f.name == mname && !f.has_self) m = &f;
                }
                if (!m) panic(e, n + " has no static '" + mname + "'");
                return call_fn(m, nullptr, eval_args_hinted(m->params),
                               pkg_of(cls->qualname));
            }
        }

        // `util.Payment.card(...)` / `util.User.named_static(...)` — the receiver is a
        // field expression the checker resolved to a type
        if (obj->kind == Expr::Kind::field && !obj->resolved.empty()) {
            if (const EnumDecl* ed = resolve_enum(obj, "")) {
                Value x;
                x.k = Value::K::enum_v;
                x.en = std::make_shared<EnumVal>();
                x.en->enum_name = ed->qualname;
                x.en->variant = mname;
                const EnumVariant* var = nullptr;
                for (const EnumVariant& v : ed->variants) {
                    if (v.name == mname) var = &v;
                }
                for (size_t i = 0; i < e->args.size(); i++) {
                    Hint h = var && i < var->payload.size()
                                 ? Hint::of(var->payload[i].type.get())
                                 : Hint();
                    x.en->payload.push_back(eval(e->args[i].get(), env, h));
                }
                return x;
            }
            if (const ClassDecl* cls = find_class(obj->resolved)) {
                const FnDecl* m = nullptr;
                for (const FnDecl& f : cls->methods) {
                    if (f.name == mname && !f.has_self) m = &f;
                }
                if (!m) panic(e, obj->resolved + " has no static '" + mname + "'");
                return call_fn(m, nullptr, eval_args_hinted(m->params),
                               pkg_of(cls->qualname));
            }
        }

        // instance call
        Value recv = eval(obj, env);
        if (recv.k == Value::K::instance && recv.inst->cls) {
            const ClassDecl* owner = recv.inst->cls;
            if (const FnDecl* m = find_method(recv.inst->cls, mname, &owner)) {
                return call_fn(m, &recv, eval_args_hinted(m->params),
                               pkg_of(owner->qualname));
            }
        }
        if (recv.k == Value::K::enum_v) {
            auto eit = enums_.find(recv.en->enum_name);
            if (eit != enums_.end()) {
                for (const FnDecl& m : eit->second->methods) {
                    if (m.name == mname) {
                        return call_fn(&m, &recv, eval_args_hinted(m.params),
                                       pkg_of(eit->second->qualname));
                    }
                }
            }
        }
        std::vector<Value> args = eval_method_args(e, env, recv, mname);
        return eval_builtin_method(e, recv, mname, args);
    }

    Value f = eval(callee, env);
    return call_value(f, eval_args_plain());
}

// ---- builtin methods --------------------------------------------------------

// ordering for sort/min/max — the checker only lets ordered element types in
static bool value_less(const Value& a, const Value& b) {
    switch (a.k) {
        case Value::K::int_: {
            if (!a.int_unsigned) return a.i < b.i;
            uint64_t ua, ub;
            std::memcpy(&ua, &a.i, sizeof ua);
            std::memcpy(&ub, &b.i, sizeof ub);
            return ua < ub;
        }
        case Value::K::float_: return a.f < b.f;
        case Value::K::decimal_: return a.dec.cmp(b.dec) < 0;
        case Value::K::string_: return *a.s < *b.s;
        case Value::K::bool_: return a.b < b.b;
        default: return false;
    }
}

// bottom-up stable merge — structurally identical to the C runtime's
// list_merge_sort, so both backends produce the same order for ANY
// predicate, even one that is not a strict weak ordering
template <typename Less>
static void stable_merge(std::vector<Value>& v, Less less) {
    size_t n = v.size();
    if (n < 2) return;
    std::vector<Value> buf(n);
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = std::min(lo + w, n), hi = std::min(lo + 2 * w, n);
            if (mid >= hi) continue;
            size_t i = lo, j = mid, o = lo;
            while (i < mid && j < hi) {
                if (!less(v[j], v[i])) buf[o++] = v[i++];
                else buf[o++] = v[j++];
            }
            while (i < mid) buf[o++] = v[i++];
            while (j < hi) buf[o++] = v[j++];
            for (size_t k = lo; k < hi; k++) v[k] = std::move(buf[k]);
        }
    }
}

// registry rows carry BT kinds, not TypeRefs — a numeric hint is all that is
// needed for a literal argument to build the Value the row expects
Interp::Hint Interp::hint_of_bt(BT p) {
    if (p == BT::dec) return Hint::decimal();
    if (p == BT::f64) return Hint::floating();
    return Hint();
}

// Evaluate builtin/container method arguments with type hints. Checked code
// already carries checker numk stamps on its number literals; these hints only
// decide literals inside re-parsed interpolation segments, which never met the
// checker. Container element hints come from the receiver's runtime values —
// best effort by design: an empty container gives no sample, and stamps cover
// every checked call anyway.
std::vector<Value> Interp::eval_method_args(const Expr* e, std::shared_ptr<Env>& env,
                                            const Value& recv, const std::string& name) {
    size_t n = e->args.size();
    std::vector<Hint> hs(n);
    auto sample = [](const Value& v) {
        if (v.k == Value::K::decimal_) return Hint::decimal();
        if (v.k == Value::K::float_) return Hint::floating();
        if (v.k == Value::K::fixed_array) {
            Hint hint;
            hint.fixed_array = true;
            return hint;
        }
        return Hint();
    };
    auto row_hints = [&](BT want) {
        for (const BuiltinMethod& b : builtin_methods()) {
            if (b.recv == want && name == b.name) {
                for (size_t i = 0; i < n && i < b.params.size(); i++) {
                    hs[i] = hint_of_bt(b.params[i]);
                }
                return;
            }
        }
    };
    switch (recv.k) {
        case Value::K::string_: row_hints(BT::str); break;
        case Value::K::bytes: row_hints(BT::bytes); break;
        case Value::K::file: row_hints(BT::file); break;
        case Value::K::mmap: row_hints(BT::mmap); break;
        case Value::K::list: {
            const auto& items = recv.list->items;
            if (!items.empty() && n >= 1) {
                Hint eh = sample(items[0]);
                if (name == "push" || name == "contains" || name == "index_of") hs[0] = eh;
                if (name == "insert" && n >= 2) hs[1] = eh;
            }
            break;
        }
        case Value::K::fixed_array: break;
        case Value::K::map: {
            const MapVal& mv = *recv.map;
            const auto& es = mv.entries;
            size_t f = 0; // first live entry — holes hold unit, not a real key
            while (f < es.size() && mv.is_dead(f)) f++;
            if (f < es.size() && n >= 1) {
                Hint kh = sample(es[f].first);
                if (name == "get" || name == "contains" || name == "remove") hs[0] = kh;
                if (name == "set") {
                    hs[0] = kh;
                    if (n >= 2) hs[1] = sample(es[f].second);
                }
            }
            break;
        }
        default: break;
    }
    std::vector<Value> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) out.push_back(eval(e->args[i].get(), env, hs[i]));
    return out;
}

Value Interp::eval_builtin_method(const Expr* e, Value& recv, const std::string& name,
                                  std::vector<Value>& args) {
    switch (recv.k) {
        case Value::K::int_:
            if (name == "abs") {
                if (recv.int_unsigned || recv.i >= 0) return recv;
                Value out = recv;
                out.i = signed_integer(0 - unsigned_integer(recv));
                normalize_integer(out);
                return out;
            }
            break;
        case Value::K::float_:
            if (name == "abs") return Value::of_float(std::fabs(recv.f));
            if (name == "round") return Value::of_int(std::llround(recv.f));
            break;
        case Value::K::decimal_:
            if (name == "abs") return Value::of_dec(recv.dec.abs());
            if (name == "round")
                return Value::of_dec(recv.dec.round_to(static_cast<int32_t>(args[0].i)));
            break;
        case Value::K::string_: {
            // string methods live in the builtin registry (builtins.cpp)
            for (const BuiltinMethod& b : builtin_methods()) {
                if (b.recv == BT::str && name == b.name) {
                    return b.run(e->line, e->col, recv, args);
                }
            }
            break;
        }
        case Value::K::bytes: {
            for (const BuiltinMethod& b : builtin_methods()) {
                if (b.recv == BT::bytes && name == b.name) {
                    return b.run(e->line, e->col, recv, args);
                }
            }
            break;
        }
        case Value::K::file: {
            for (const BuiltinMethod& b : builtin_methods()) {
                if (b.recv == BT::file && name == b.name) {
                    return b.run(e->line, e->col, recv, args);
                }
            }
            break;
        }
        case Value::K::mmap: {
            for (const BuiltinMethod& b : builtin_methods()) {
                if (b.recv == BT::mmap && name == b.name) {
                    return b.run(e->line, e->col, recv, args);
                }
            }
            break;
        }
        case Value::K::list: {
            auto& items = recv.list->items;
            if (name == "clone") {
                Value out;
                out.k = Value::K::list;
                out.list = std::make_shared<ListVal>();
                out.list->items = items;
                return out;
            }
            if (name == "push") { items.push_back(args[0]); return Value::unit(); }
            if (name == "pop") {
                if (items.empty()) return none();
                Value v = items.back();
                items.pop_back();
                return some(std::move(v));
            }
            if (name == "get") {
                int64_t i = args[0].i;
                if (i < 0 || static_cast<size_t>(i) >= items.size()) return none();
                return some(items[static_cast<size_t>(i)]);
            }
            if (name == "len") return Value::of_int(static_cast<int64_t>(items.size()));
            if (name == "contains") {
                for (const Value& v : items) {
                    if (value_eq(v, args[0])) return Value::of_bool(true);
                }
                return Value::of_bool(false);
            }
            if (name == "max") {
                if (items.empty()) return none();
                Value best = items[0];
                for (const Value& v : items) {
                    bool bigger = false;
                    if (v.k == Value::K::int_) bigger = v.i > best.i;
                    else if (v.k == Value::K::float_) bigger = v.f > best.f;
                    else if (v.k == Value::K::decimal_) bigger = v.dec.cmp(best.dec) > 0;
                    else if (v.k == Value::K::string_) bigger = *v.s > *best.s;
                    if (bigger) best = v;
                }
                return some(std::move(best));
            }
            if (name == "join") {
                std::string out;
                for (size_t i = 0; i < items.size(); i++) {
                    if (i) out += *args[0].s;
                    out += display(items[i]);
                }
                return Value::of_str(std::move(out));
            }
            if (name == "reserve") {
                if (args[0].i < 0) {
                    panic(e, "negative reserve capacity " + std::to_string(args[0].i));
                }
                if (args[0].i > (1LL << 58)) panic(e, "reserve capacity too large");
                items.reserve(static_cast<size_t>(args[0].i));
                return Value::unit();
            }
            if (name == "first") {
                if (items.empty()) return none();
                return some(items.front());
            }
            if (name == "last") {
                if (items.empty()) return none();
                return some(items.back());
            }
            if (name == "min") {
                if (items.empty()) return none();
                Value best = items[0];
                for (const Value& v : items) {
                    if (value_less(v, best)) best = v;
                }
                return some(std::move(best));
            }
            if (name == "index_of") {
                for (size_t i = 0; i < items.size(); i++) {
                    if (value_eq(items[i], args[0])) {
                        return some(Value::of_int(static_cast<int64_t>(i)));
                    }
                }
                return none();
            }
            if (name == "insert") {
                int64_t i = args[0].i;
                if (i < 0 || i > static_cast<int64_t>(items.size())) {
                    panic(e, "insert at " + std::to_string(i) + " out of range (len " +
                                 std::to_string(items.size()) + ")");
                }
                items.insert(items.begin() + i, args[1]);
                return Value::unit();
            }
            if (name == "remove") {
                int64_t i = args[0].i;
                if (i < 0 || i >= static_cast<int64_t>(items.size())) {
                    panic(e, "list index " + std::to_string(i) + " out of range (len " +
                                 std::to_string(items.size()) + ")");
                }
                Value v = std::move(items[static_cast<size_t>(i)]);
                items.erase(items.begin() + i);
                return v;
            }
            if (name == "reverse") {
                std::reverse(items.begin(), items.end());
                return Value::unit();
            }
            if (name == "clear") {
                items.clear();
                return Value::unit();
            }
            if (name == "slice") {
                int64_t from = args[0].i, to = args[1].i;
                if (from < 0 || to < from || to > static_cast<int64_t>(items.size())) {
                    panic(e, "slice " + std::to_string(from) + ".." + std::to_string(to) +
                                 " out of range (len " + std::to_string(items.size()) + ")");
                }
                Value out;
                out.k = Value::K::list;
                out.list = std::make_shared<ListVal>();
                out.list->items.assign(items.begin() + from, items.begin() + to);
                return out;
            }
            if (name == "sort") {
                stable_merge(items, value_less);
                return Value::unit();
            }
            if (name == "sort_by") {
                Value f = args[0];
                stable_merge(items, [&](const Value& a, const Value& b) -> bool {
                    if (f.k == Value::K::closure) {
                        return call_closure(*f.clo, {a, b}).b;
                    }
                    return call_fn(f.fnr->decl, nullptr, {a, b},
                                   pkg_of(f.fnr->decl->qualname))
                        .b;
                });
                return Value::unit();
            }
            if (name == "sort_by_key") {
                Value f = args[0];
                std::vector<std::pair<int64_t, Value>> keyed;
                keyed.reserve(items.size());
                for (Value& item : items) {
                    Value key = f.k == Value::K::closure
                                    ? call_closure(*f.clo, {item})
                                    : call_fn(f.fnr->decl, nullptr, {item},
                                              pkg_of(f.fnr->decl->qualname));
                    keyed.emplace_back(key.i, std::move(item));
                }
                std::stable_sort(keyed.begin(), keyed.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.first < right.first;
                                 });
                for (size_t i = 0; i < keyed.size(); i++)
                    items[i] = std::move(keyed[i].second);
                return Value::unit();
            }
            break;
        }
        case Value::K::map: {
            MapVal& mv = *recv.map;
            auto& entries = mv.entries;
            if (name == "clone") {
                Value out;
                out.k = Value::K::map;
                out.map = std::make_shared<MapVal>();
                out.map->entries = mv.entries;
                out.map->index = mv.index;
                out.map->tombs = mv.tombs;
                out.map->dead = mv.dead;
                out.map->holes = mv.holes;
                out.map->ordered = mv.ordered;
                return out;
            }
            if (name == "get") {
                uint64_t h = 0;
                size_t i = map_find(mv, args[0], h);
                if (i != SIZE_MAX) return some(entries[i].second);
                return none();
            }
            if (name == "set") {
                map_set(mv, args[0], args[1]);
                return Value::unit();
            }
            if (name == "insert") {
                uint64_t h = 0;
                if (map_find(mv, args[0], h) != SIZE_MAX)
                    return Value::of_bool(false);
                map_append(mv, h, args[0], args[1]);
                return Value::of_bool(true);
            }
            if (name == "reserve") {
                if (args[0].i < 0) {
                    panic(e, "negative reserve capacity " + std::to_string(args[0].i));
                }
                if (args[0].i > (1LL << 58)) panic(e, "reserve capacity too large");
                const size_t capacity = static_cast<size_t>(args[0].i);
                entries.reserve(capacity);
                map_reindex(mv, capacity);
                return Value::unit();
            }
            if (name == "len") return Value::of_int(static_cast<int64_t>(mv.live()));
            if (name == "contains") {
                uint64_t h = 0;
                return Value::of_bool(map_find(mv, args[0], h) != SIZE_MAX);
            }
            if (name == "remove") {
                return Value::of_bool(map_remove_key(mv, args[0]));
            }
            if (name == "keys" || name == "values") {
                Value out;
                out.k = Value::K::list;
                out.list = std::make_shared<ListVal>();
                for (size_t i = 0; i < entries.size(); i++) {
                    if (mv.is_dead(i)) continue;
                    out.list->items.push_back(name == "keys" ? entries[i].first
                                                             : entries[i].second);
                }
                return out;
            }
            if (name == "clear") {
                entries.clear();
                mv.index.clear();
                mv.tombs = 0;
                mv.dead.clear();
                mv.holes = 0;
                return Value::unit();
            }
            break;
        }
        case Value::K::box:
            if (name == "get") return recv.box->inner;
            if (name == "set") {
                recv.box->inner = std::move(args[0]);
                return Value::unit();
            }
            break;
        case Value::K::arena:
            if (name == "put") {
                int64_t handle = static_cast<int64_t>(recv.arena->values.size());
                recv.arena->values.push_back(std::move(args[0]));
                return Value::of_int(handle);
            }
            if (name == "get") {
                int64_t handle = args[0].i;
                if (handle < 0 || static_cast<size_t>(handle) >= recv.arena->values.size())
                    return none();
                return some(recv.arena->values[static_cast<size_t>(handle)]);
            }
            if (name == "at") {
                int64_t handle = args[0].i;
                if (handle < 0 || static_cast<size_t>(handle) >= recv.arena->values.size()) {
                    panic(e, "arena handle " + std::to_string(handle) +
                                 " out of range (len " +
                                 std::to_string(recv.arena->values.size()) + ")");
                }
                return recv.arena->values[static_cast<size_t>(handle)];
            }
            if (name == "len")
                return Value::of_int(static_cast<int64_t>(recv.arena->values.size()));
            if (name == "clear") {
                recv.arena->values.clear();
                return Value::unit();
            }
            break;
        case Value::K::shared:
            if (name == "get") return recv.shared->inner;
            if (name == "downgrade") {
                Value out;
                out.k = Value::K::weak;
                out.weak = std::make_shared<WeakVal>();
                out.weak->inner = recv.shared;
                return out;
            }
            break;
        case Value::K::weak:
            if (name == "upgrade") {
                std::shared_ptr<SharedVal> strong = recv.weak->inner.lock();
                if (!strong) return none();
                Value out;
                out.k = Value::K::shared;
                out.shared = std::move(strong);
                return some(std::move(out));
            }
            if (name == "expired") return Value::of_bool(recv.weak->inner.expired());
            break;
        case Value::K::enum_v: {
            const std::string& variant = recv.en->variant;
            bool has = variant == "some" || variant == "ok";
            auto invoke = [&](Value function, Value argument) {
                if (function.k == Value::K::closure)
                    return call_closure(*function.clo, {std::move(argument)});
                if (function.k == Value::K::fn_ref)
                    return call_fn(function.fnr->decl, nullptr, {std::move(argument)},
                                   pkg_of(function.fnr->decl->qualname));
                panic(e, "not callable");
            };
            if (name == "map") {
                if (!has || recv.en->payload.empty()) return recv;
                Value mapped = invoke(args[0], recv.en->payload[0]);
                Value out;
                out.k = Value::K::enum_v;
                out.en = std::make_shared<EnumVal>();
                out.en->enum_name = recv.en->enum_name;
                out.en->variant = variant;
                out.en->payload.push_back(std::move(mapped));
                return out;
            }
            if (name == "and_then") {
                if (!has || recv.en->payload.empty()) return recv;
                return invoke(args[0], recv.en->payload[0]);
            }
            if (name == "filter") {
                if (variant != "some" || recv.en->payload.empty()) return recv;
                return invoke(args[0], recv.en->payload[0]).b ? recv : none();
            }
            if (name == "recover") {
                if (variant == "ok" && !recv.en->payload.empty())
                    return recv.en->payload[0];
                if (variant == "err" && !recv.en->payload.empty())
                    return invoke(args[0], recv.en->payload[0]);
            }
            if (name == "or") {
                if (has && !recv.en->payload.empty()) return recv.en->payload[0];
                return args[0];
            }
            if (name == "expect") {
                if (has && !recv.en->payload.empty()) return recv.en->payload[0];
                panic(e, *args[0].s);
            }
            if (name == "is_some") return Value::of_bool(variant == "some");
            if (name == "is_none") return Value::of_bool(variant == "none");
            if (name == "is_ok") return Value::of_bool(variant == "ok");
            break;
        }
        case Value::K::thread: {
            if (name == "join") {
                if (recv.thread->joined) panic(e, "thread already joined");
                recv.thread->joined = true;
                if (recv.thread->th.joinable()) recv.thread->th.join();
                if (!recv.thread->panic->empty()) {
                    panic(e, "thread panicked: " + *recv.thread->panic);
                }
                return *recv.thread->result;
            }
            break;
        }
        case Value::K::mutex: {
            if (name == "with") {
                std::lock_guard<std::mutex> lock(recv.mutex->m);
                Value f = args[0];
                if (f.k == Value::K::closure) {
                    call_closure(*f.clo, {*recv.mutex->inner});
                }
                return Value::unit();
            }
            break;
        }
        case Value::K::channel: {
            ChannelVal& ch = *recv.chan;
            if (name == "send") {
                std::unique_lock<std::mutex> lock(ch.m);
                ch.cv_send.wait(lock, [&] { return ch.q.size() < ch.cap || ch.closed; });
                if (ch.closed) panic(e, "send on a closed channel");
                ch.q.push_back(args[0]);
                ch.cv_recv.notify_one();
                return Value::unit();
            }
            if (name == "recv") {
                std::unique_lock<std::mutex> lock(ch.m);
                ch.cv_recv.wait(lock, [&] { return !ch.q.empty() || ch.closed; });
                if (ch.q.empty()) return none();
                Value v = ch.q.front();
                ch.q.pop_front();
                ch.cv_send.notify_one();
                return some(std::move(v));
            }
            if (name == "close") {
                std::lock_guard<std::mutex> lock(ch.m);
                ch.closed = true;
                ch.cv_send.notify_all();
                ch.cv_recv.notify_all();
                return Value::unit();
            }
            break;
        }
        case Value::K::atomic: {
            if (name == "add") return Value::of_int(recv.atomic->v.fetch_add(args[0].i) + args[0].i);
            if (name == "get") return Value::of_int(recv.atomic->v.load());
            if (name == "set") { recv.atomic->v.store(args[0].i); return Value::unit(); }
            break;
        }
        case Value::K::slice: {
            auto element_at = [&](int64_t index) {
                if (index < 0 || index >= recv.slice_len) {
                    panic(e, "slice index " + std::to_string(index) +
                                 " out of range (len " +
                                 std::to_string(recv.slice_len) + ")");
                }
                RawPtrVal pointer = recv.slice_ptr;
                pointer.address = static_cast<void*>(
                    static_cast<char*>(pointer.address) + index * pointer.size);
                return pointer;
            };
            if (name == "len") return Value::of_int(recv.slice_len);
            if (name == "get") return raw_read(element_at(args[0].i));
            if (name == "set") {
                raw_write(element_at(args[0].i), args[1]);
                return Value::unit();
            }
            if (name == "subslice") {
                int64_t from = args[0].i, to = args[1].i;
                if (from < 0 || to < from || to > recv.slice_len)
                    panic(e, "slice range out of bounds");
                Value out = recv;
                out.slice_ptr.address = static_cast<void*>(
                    static_cast<char*>(recv.slice_ptr.address) + from * recv.slice_ptr.size);
                out.slice_len = to - from;
                return out;
            }
            if (name == "as_ptr") {
                Value out;
                out.k = Value::K::rawptr;
                out.raw = recv.slice_ptr;
                return out;
            }
            break;
        }
        case Value::K::rawptr: {
            auto require_atomic_alignment = [&] {
                uintptr_t address = reinterpret_cast<uintptr_t>(recv.raw.address);
                if (recv.raw.align > 1 && address % recv.raw.align != 0)
                    panic(e, "unaligned raw pointer atomic access");
            };
            auto word = [](const Value& value) {
                return value.k == Value::K::bool_ ? uint64_t(value.b ? 1 : 0)
                                                   : unsigned_integer(value);
            };
            auto scalar = [&](uint64_t bits) {
                if (recv.raw.scalar == RawScalar::boolean)
                    return Value::of_bool(bits != 0);
                Value out = Value::of_int(signed_integer(bits));
                out.int_bits = recv.raw.bits;
                out.int_unsigned = recv.raw.scalar == RawScalar::unsigned_int;
                normalize_integer(out);
                return out;
            };
            if (name == "read" || name == "read_volatile") {
                if (!recv.raw.address) panic(e, "null raw pointer read");
                return raw_read(recv.raw);
            }
            if (name == "write" || name == "write_volatile") {
                if (!recv.raw.address) panic(e, "null raw pointer write");
                raw_write(recv.raw, args[0]);
                return Value::unit();
            }
            if (name == "offset") {
                Value out = recv;
                out.raw.address = static_cast<void*>(
                    static_cast<char*>(recv.raw.address) + args[0].i * recv.raw.size);
                return out;
            }
            if (name == "address") {
                uint64_t bits = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(recv.raw.address));
                Value out = Value::of_int(signed_integer(bits));
                out.int_unsigned = true;
                return out;
            }
            if (name == "is_null") return Value::of_bool(!recv.raw.address);
            if (name == "element_size") return Value::of_int(recv.raw.size);
            if (name == "element_align") return Value::of_int(recv.raw.align);
            if (name == "copy_from") {
                if (args[1].i < 0) panic(e, "negative raw copy count");
                uint64_t count = static_cast<uint64_t>(args[1].i);
                if (!recv.raw.size || count > (uint64_t{1} << 58) / recv.raw.size)
                    panic(e, "raw copy too large");
                if (count && (!recv.raw.address || !args[0].raw.address))
                    panic(e, "null raw pointer copy");
                std::memmove(recv.raw.address, args[0].raw.address,
                             static_cast<size_t>(count * recv.raw.size));
                return Value::unit();
            }
            if (name == "fill_zero") {
                if (args[0].i < 0) panic(e, "negative raw zero count");
                uint64_t count = static_cast<uint64_t>(args[0].i);
                if (!recv.raw.size || count > (uint64_t{1} << 58) / recv.raw.size)
                    panic(e, "raw zero too large");
                if (count && !recv.raw.address) panic(e, "null raw pointer zero");
                std::memset(recv.raw.address, 0,
                            static_cast<size_t>(count * recv.raw.size));
                return Value::unit();
            }
            if (name == "atomic_load") {
                if (!recv.raw.address) panic(e, "null raw pointer atomic load");
                require_atomic_alignment();
                return scalar(raw_atomic_load(recv.raw));
            }
            if (name == "atomic_store") {
                if (!recv.raw.address) panic(e, "null raw pointer atomic store");
                require_atomic_alignment();
                raw_atomic_store(recv.raw, word(args[0]));
                return Value::unit();
            }
            if (name == "atomic_fetch_add") {
                if (!recv.raw.address) panic(e, "null raw pointer atomic fetch_add");
                require_atomic_alignment();
                return scalar(raw_atomic_fetch_add(recv.raw, word(args[0])));
            }
            if (name == "atomic_compare_exchange") {
                if (!recv.raw.address)
                    panic(e, "null raw pointer atomic compare_exchange");
                require_atomic_alignment();
                return Value::of_bool(raw_atomic_compare_exchange(
                    recv.raw, word(args[0]), word(args[1])));
            }
            if (name == "free") {
                std::free(recv.raw.address);
                return Value::unit();
            }
            break;
        }
        case Value::K::simd4f32: {
            if (name == "lane") {
                if (args[0].i < 0 || args[0].i >= 4)
                    panic(e, "SIMD lane out of range");
                Value out = Value::of_float(recv.simd[static_cast<size_t>(args[0].i)]);
                out.float_bits = 32;
                return out;
            }
            if (name == "sum") {
                float first = recv.simd[0] + recv.simd[1];
                float second = recv.simd[2] + recv.simd[3];
                Value out = Value::of_float(first + second);
                out.float_bits = 32;
                return out;
            }
            if (name == "store") {
                if (!args[0].raw.address) panic(e, "null SIMD store");
                std::memcpy(args[0].raw.address, recv.simd.data(),
                            sizeof(float) * recv.simd.size());
                return Value::unit();
            }
            break;
        }
        case Value::K::fixed_array:
            if (name == "len") return Value::of_int(static_cast<int64_t>(recv.array.size()));
            break;
        default:
            break;
    }
    panic(e, "no method '" + name + "' here");
}

// ---- match ------------------------------------------------------------------

bool Interp::match_pattern(const Pattern* p, const Value& v, Env& bind_env,
                           std::shared_ptr<Env>& outer) {
    switch (p->kind) {
        case Pattern::Kind::wildcard:
            return true;
        case Pattern::Kind::alt:
            for (const PatPtr& a : p->alts) {
                if (match_pattern(a.get(), v, bind_env, outer)) return true;
            }
            return false;
        case Pattern::Kind::literal: {
            Hint h;
            if (v.k == Value::K::decimal_) h = Hint::decimal();
            if (v.k == Value::K::float_) h = Hint::floating();
            Value lit = eval(p->lit.get(), outer, h);
            return value_eq(lit, v);
        }
        case Pattern::Kind::range: {
            Value lo = eval(p->lit.get(), outer);
            Value hi = eval(p->lit2.get(), outer);
            if (v.k != Value::K::int_) return false;
            return v.i >= lo.i && (p->inclusive ? v.i <= hi.i : v.i < hi.i);
        }
        case Pattern::Kind::name: {
            if (v.k != Value::K::enum_v || v.en->variant != p->name) return false;
            for (size_t i = 0; i < p->bindings.size() && i < v.en->payload.size(); i++) {
                bind_env.declare(p->bindings[i].name, v.en->payload[i]);
            }
            return true;
        }
    }
    return false;
}

Value Interp::eval_match(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    Value subj = eval(e->subject.get(), env);
    for (const MatchArm& arm : e->arms) {
        auto arm_env = std::make_shared<Env>();
        arm_env->parent = env;
        if (match_pattern(arm.pat.get(), subj, *arm_env, env)) {
            if (arm.is_block) {
                exec_block(arm.body, arm_env);
                return Value::unit();
            }
            return eval(arm.value.get(), arm_env, hint);
        }
    }
    panic(e, "no match arm fit the value " + display(subj));
}

// ---- strings ----------------------------------------------------------------

const std::vector<Interp::StrPart>& Interp::string_parts(const Expr* e) {
    std::lock_guard<std::mutex> lock(str_cache_mu_);
    auto it = str_cache_.find(e);
    if (it != str_cache_.end()) return it->second;

    std::vector<StrPart> parts;
    std::string_view raw = e->text;
    std::string_view body = raw.size() >= 2 ? raw.substr(1, raw.size() - 2)
                                            : std::string_view{};
    std::string cur;
    size_t i = 0;
    while (i < body.size()) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            char n = body[i + 1];
            switch (n) {
                case 'n': cur += '\n'; break;
                case 't': cur += '\t'; break;
                case 'r': cur += '\r'; break;
                case '0': cur += '\0'; break;
                case '\\': cur += '\\'; break;
                case '"': cur += '"'; break;
                case '{': cur += '{'; break;
                case '}': cur += '}'; break;
                default: cur += n; break;
            }
            i += 2;
            continue;
        }
        if (c == '{') {
            size_t start = i + 1;
            int depth = 1;
            size_t j = start;
            bool in_str = false;
            while (j < body.size() && depth > 0) {
                char d = body[j];
                if (d == '\\') { j += 2; continue; }
                if (in_str) {
                    if (d == '"') in_str = false;
                } else if (d == '"') {
                    in_str = true;
                } else if (d == '{') {
                    depth += 1;
                } else if (d == '}') {
                    depth -= 1;
                }
                j += 1;
            }
            std::string segment(body.substr(start, j - 1 - start));
            if (!cur.empty()) {
                StrPart p;
                p.text = cur;
                parts.push_back(std::move(p));
                cur.clear();
            }
            StrPart p;
            std::string expr_text(split_fmt_spec(segment, p.spec, nullptr));
            // the parsed expr holds string_views into the segment text,
            // so the part must own that text for as long as it lives
            p.src = std::make_shared<std::string>(std::move(expr_text));
            Lexer lx(*p.src);
            Parser ps(lx.scan_all());
            p.expr = ps.parse_standalone_expr();
            parts.push_back(std::move(p));
            i = j;
            continue;
        }
        cur += c;
        i += 1;
    }
    if (!cur.empty()) {
        StrPart p;
        p.text = cur;
        parts.push_back(std::move(p));
    }
    return str_cache_.emplace(e, std::move(parts)).first->second;
}

Value Interp::eval_string(const Expr* e, std::shared_ptr<Env>& env) {
    // fast path: no braces at all
    std::string_view raw = e->text;
    bool has_brace = false;
    for (char c : raw) has_brace |= c == '{';
    if (!has_brace) {
        const std::vector<StrPart>& parts = string_parts(e);
        return Value::of_str(parts.empty() ? "" : parts[0].text);
    }
    const std::vector<StrPart>& parts = string_parts(e);
    std::string out;
    for (const StrPart& p : parts) {
        if (p.expr) {
            Value v = eval(p.expr.get(), env);
            std::string piece;
            if (p.spec.has && p.spec.places >= 0 && v.k == Value::K::float_) {
                piece = fmt_float_text(v.f, p.spec.places);
            } else if (p.spec.has && p.spec.places >= 0 &&
                       v.k == Value::K::decimal_) {
                piece = fmt_dec_text(v.dec, p.spec.places);
            } else {
                piece = display(v);
            }
            if (p.spec.has && p.spec.width > 0) {
                piece = fmt_pad_text(std::move(piece), p.spec.width, p.spec.left);
            }
            out += piece;
        } else {
            out += p.text;
        }
    }
    return Value::of_str(std::move(out));
}

// ---- misc -------------------------------------------------------------------

bool Interp::value_eq(const Value& a, const Value& b) {
    if (a.k != b.k) return false;
    switch (a.k) {
        case Value::K::unit: return true;
        case Value::K::int_: return a.i == b.i;
        case Value::K::float_: return a.f == b.f;
        case Value::K::decimal_: return a.dec.cmp(b.dec) == 0;
        case Value::K::bool_: return a.b == b.b;
        case Value::K::string_: return *a.s == *b.s;
        case Value::K::enum_v: {
            if (a.en->enum_name != b.en->enum_name || a.en->variant != b.en->variant)
                return false;
            if (a.en->payload.size() != b.en->payload.size()) return false;
            for (size_t i = 0; i < a.en->payload.size(); i++) {
                if (!value_eq(a.en->payload[i], b.en->payload[i])) return false;
            }
            return true;
        }
        case Value::K::instance: return a.inst == b.inst;
        case Value::K::list: return a.list == b.list;
        case Value::K::box: return a.box == b.box;
        case Value::K::arena: return a.arena == b.arena;
        case Value::K::shared: return a.shared == b.shared;
        case Value::K::weak: return a.weak == b.weak;
        case Value::K::rawptr: return a.raw.address == b.raw.address;
        case Value::K::fixed_array:
            if (a.array.size() != b.array.size()) return false;
            for (size_t i = 0; i < a.array.size(); i++)
                if (!value_eq(a.array[i], b.array[i])) return false;
            return true;
        case Value::K::struct_v:
            if (a.struct_decl != b.struct_decl ||
                a.struct_fields.size() != b.struct_fields.size())
                return false;
            for (size_t i = 0; i < a.struct_fields.size(); i++)
                if (!value_eq(a.struct_fields[i], b.struct_fields[i])) return false;
            return true;
        case Value::K::bytes: return a.bytes->data == b.bytes->data;
        default: return false;
    }
}

// ---- map index ---------------------------------------------------------------
// Entries stay flat and the index maps hash -> position. OrderedMap preserves
// this vector's insertion order. Plain Map may swap-remove. Small maps skip
// the index and scan.

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static uint64_t fnv_hash(const void* p, size_t n) {
    const unsigned char* s = static_cast<const unsigned char*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return mix64(h);
}

// must agree with value_eq arm for arm: whatever compares equal must hash
// equal, or the index misses keys the old linear scan found
uint64_t Interp::value_hash(const Value& v) {
    switch (v.k) {
        case Value::K::unit: return 0;
        case Value::K::int_: return mix64(static_cast<uint64_t>(v.i));
        case Value::K::bool_: return mix64(v.b ? 1 : 0);
        case Value::K::float_: {
            double d = v.f == 0.0 ? 0.0 : v.f; // -0.0 == 0.0, so same hash
            uint64_t bits;
            std::memcpy(&bits, &d, 8);
            return mix64(bits);
        }
        case Value::K::string_: return fnv_hash(v.s->data(), v.s->size());
        case Value::K::decimal_: {
            // cmp() aligns scales, so 2.50 == 2.5 — hash the canonical form
            __int128 c = v.dec.coeff;
            int32_t s = v.dec.scale;
            while (s > 0 && c % 10 == 0) {
                c /= 10;
                s -= 1;
            }
            uint64_t lo = static_cast<uint64_t>(static_cast<unsigned __int128>(c));
            uint64_t hi = static_cast<uint64_t>(static_cast<unsigned __int128>(c) >> 64);
            return mix64(lo ^ mix64(hi ^ static_cast<uint32_t>(s)));
        }
        case Value::K::enum_v: {
            uint64_t h = fnv_hash(v.en->enum_name.data(), v.en->enum_name.size());
            h = mix64(h ^ fnv_hash(v.en->variant.data(), v.en->variant.size()));
            for (const Value& p : v.en->payload) h = mix64(h ^ value_hash(p));
            return h;
        }
        case Value::K::instance:
            return mix64(reinterpret_cast<uintptr_t>(v.inst.get()));
        case Value::K::list:
            return mix64(reinterpret_cast<uintptr_t>(v.list.get()));
        case Value::K::box:
            return mix64(reinterpret_cast<uintptr_t>(v.box.get()));
        case Value::K::arena:
            return mix64(reinterpret_cast<uintptr_t>(v.arena.get()));
        case Value::K::shared:
            return mix64(reinterpret_cast<uintptr_t>(v.shared.get()));
        case Value::K::weak:
            return mix64(reinterpret_cast<uintptr_t>(v.weak.get()));
        case Value::K::rawptr:
            return mix64(reinterpret_cast<uintptr_t>(v.raw.address));
        case Value::K::bytes:
            return fnv_hash(v.bytes->data.data(), v.bytes->data.size());
        case Value::K::fixed_array: {
            uint64_t h = mix64(v.array.size());
            for (const Value& element : v.array)
                h = h * 1099511628211ull ^ value_hash(element);
            return h;
        }
        case Value::K::struct_v: {
            uint64_t h = mix64(v.struct_fields.size());
            for (const Value& field : v.struct_fields)
                h = h * 1099511628211ull ^ value_hash(field);
            return h;
        }
        default: return 0; // value_eq's never-equal arm: any hash is consistent
    }
}

// position of key in m.entries, or SIZE_MAX. h is filled in iff the index is
// active, so map_append can reuse it without hashing twice; *slot_out gets
// the hit's index slot so remove can tombstone it O(1).
size_t Interp::map_find(MapVal& m, const Value& key, uint64_t& h,
                        size_t* slot_out) {
    if (m.index.empty()) {
        for (size_t i = 0; i < m.entries.size(); i++) {
            if (value_eq(m.entries[i].first, key)) return i;
        }
        return SIZE_MAX;
    }
    h = value_hash(key);
    uint64_t mask = m.index.size() - 1;
    uint64_t frag = h & IDX_FRAG;
    for (uint64_t i = h & mask;; i = (i + 1) & mask) {
        uint64_t w = m.index[i];
        uint64_t st = w & IDX_POS;
        if (st == 0) return SIZE_MAX;
        if (st >= 2 && (w & IDX_FRAG) == frag) {
            size_t pos = static_cast<size_t>(st) - 2;
            if (value_eq(m.entries[pos].first, key)) {
                if (slot_out) *slot_out = static_cast<size_t>(i);
                return pos;
            }
        }
    }
}

// (re)build the index sized for the current entry count, dropping tombstones
// and compacting holes
void Interp::map_reindex(MapVal& m, size_t reserve) {
    if (!m.dead.empty()) {
        size_t w = 0;
        for (size_t p = 0; p < m.entries.size(); p++) {
            if (m.dead[p]) continue;
            if (w != p) m.entries[w] = std::move(m.entries[p]);
            w += 1;
        }
        m.entries.resize(w);
        m.dead.clear();
        m.holes = 0;
    }
    size_t cap = 16;
    const size_t wanted = std::max(m.entries.size(), reserve);
    while (wanted * 3 >= cap * 2) cap <<= 1;
    m.index.assign(cap, 0);
    m.tombs = 0;
    uint64_t mask = cap - 1;
    for (size_t pos = 0; pos < m.entries.size(); pos++) {
        uint64_t h = value_hash(m.entries[pos].first);
        uint64_t i = h & mask;
        while (m.index[i] & IDX_POS) i = (i + 1) & mask;
        m.index[i] = (h & IDX_FRAG) | static_cast<uint64_t>(pos + 2);
    }
}

// append an entry whose key is known absent (h from the map_find that missed)
void Interp::map_append(MapVal& m, uint64_t h, Value key, Value val) {
    m.entries.emplace_back(std::move(key), std::move(val));
    if (!m.dead.empty()) m.dead.push_back(0);
    if (m.index.empty()) {
        if (m.entries.size() > MAP_LINEAR_MAX) map_reindex(m);
        return;
    }
    if ((m.entries.size() + m.tombs) * 3 >= m.index.size() * 2) {
        map_reindex(m);
        return;
    }
    uint64_t mask = m.index.size() - 1;
    uint64_t i = h & mask;
    while ((m.index[i] & IDX_POS) >= 2) i = (i + 1) & mask;
    if ((m.index[i] & IDX_POS) == 1) m.tombs -= 1;
    m.index[i] = (h & IDX_FRAG) | static_cast<uint64_t>(m.entries.size() + 1);
}

void Interp::map_set(MapVal& m, Value key, Value val) {
    uint64_t h = 0;
    size_t i = map_find(m, key, h);
    if (i != SIZE_MAX) {
        m.entries[i].second = std::move(val); // key kept, duplicate dropped
        return;
    }
    map_append(m, h, std::move(key), std::move(val));
}

// number-literal keys inside re-parsed interpolation segments never met the
// checker (its numk stamps cover every checked read); build them as the map's
// key kind, sampled from the first live entry
Interp::Hint Interp::map_key_hint(const MapVal& m) {
    for (size_t i = 0; i < m.entries.size(); i++) {
        if (m.is_dead(i)) continue;
        if (m.entries[i].first.k == Value::K::decimal_) return Hint::decimal();
        if (m.entries[i].first.k == Value::K::float_) return Hint::floating();
        if (m.entries[i].first.k == Value::K::fixed_array) {
            Hint hint;
            hint.fixed_array = true;
            return hint;
        }
        break;
    }
    return {};
}

bool Interp::map_remove_key(MapVal& m, const Value& key) {
    uint64_t h = 0;
    size_t slot = 0;
    size_t i = map_find(m, key, h, &slot);
    if (i == SIZE_MAX) return false;
    if (m.index.empty()) {
        if (m.ordered) {
            m.entries.erase(m.entries.begin() + static_cast<ptrdiff_t>(i));
        } else {
            size_t last = m.entries.size() - 1;
            if (i != last) m.entries[i] = std::move(m.entries[last]);
            m.entries.pop_back();
        }
        return true;
    }
    if (!m.ordered) {
        size_t last = m.entries.size() - 1;
        m.index[slot] = 1;
        m.tombs += 1;
        if (i != last) {
            uint64_t moved_hash = value_hash(m.entries[last].first);
            uint64_t mask = m.index.size() - 1;
            for (uint64_t at = moved_hash & mask;; at = (at + 1) & mask) {
                uint64_t state = m.index[at] & IDX_POS;
                if (state == static_cast<uint64_t>(last + 2)) {
                    m.index[at] = (m.index[at] & IDX_FRAG) |
                                  static_cast<uint64_t>(i + 2);
                    break;
                }
            }
            m.entries[i] = std::move(m.entries[last]);
        }
        m.entries.pop_back();
        if (m.tombs > m.entries.size()) map_reindex(m);
        return true;
    }
    // indexed: reset the pair into a hole — no entry moves, so no index
    // position needs fixing and delete is O(1). Reindex compacts once
    // holes outnumber live entries, so the cost is amortized.
    m.entries[i] = {Value(), Value()};
    if (m.dead.empty()) m.dead.assign(m.entries.size(), 0);
    m.dead[i] = 1;
    m.holes += 1;
    m.index[slot] = 1; // map_find landed on the hit's slot
    m.tombs += 1;
    if (m.entries.size() > m.live() * 2) map_reindex(m);
    return true;
}

std::string Interp::display(const Value& v) {
    // iterative with an explicit work stack: enums and lists nest to data
    // depth (a 400k-link chain), and a recursive printer smashed the C++
    // stack. Teardown is already iterative; the printer must be too.
    struct Item {
        const Value* val; // null = emit text
        const char* text;
        size_t tlen;
    };
    std::string out;
    std::vector<Item> work;
    work.push_back({&v, nullptr, 0});
    while (!work.empty()) {
        Item it = work.back();
        work.pop_back();
        if (!it.val) {
            out.append(it.text, it.tlen);
            continue;
        }
        const Value& x = *it.val;
        switch (x.k) {
            case Value::K::enum_v: {
                const auto& pay = x.en->payload;
                if (pay.empty()) {
                    out += x.en->variant;
                    break;
                }
                out += x.en->variant;
                out += '(';
                // push ")" then payloads in reverse so they pop in order
                work.push_back({nullptr, ")", 1});
                for (size_t i = pay.size(); i-- > 1;) {
                    work.push_back({&pay[i], nullptr, 0});
                    work.push_back({nullptr, ", ", 2});
                }
                work.push_back({&pay[0], nullptr, 0});
                break;
            }
            case Value::K::list: {
                const auto& items = x.list->items;
                out += '[';
                work.push_back({nullptr, "]", 1});
                if (!items.empty()) {
                    for (size_t i = items.size(); i-- > 1;) {
                        work.push_back({&items[i], nullptr, 0});
                        work.push_back({nullptr, ", ", 2});
                    }
                    work.push_back({&items[0], nullptr, 0});
                }
                break;
            }
            default:
                out += display_scalar(x);
                break;
        }
    }
    return out;
}

std::string Interp::display_scalar(const Value& v) { // static
    switch (v.k) {
        case Value::K::enum_v:
        case Value::K::list:
            return "?"; // handled iteratively in display(); never reached
        case Value::K::unit: return "()";
        case Value::K::int_:
            return v.int_unsigned ? std::to_string(unsigned_integer(v))
                                  : std::to_string(v.i);
        case Value::K::float_: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.10g", v.f);
            return buf;
        }
        case Value::K::decimal_: return v.dec.to_string();
        case Value::K::bool_: return v.b ? "true" : "false";
        case Value::K::string_: return *v.s;
        case Value::K::instance:
            return v.inst->cls ? v.inst->cls->name : "Error";
        case Value::K::struct_v:
            return v.struct_decl ? v.struct_decl->name : "{struct}";
        case Value::K::map: return "{map}";
        case Value::K::bytes: return "{bytes}";
        case Value::K::file: return "{file}";
        case Value::K::mmap: return "{mmap}";
        case Value::K::closure: return "{fn}";
        case Value::K::fn_ref: return "{fn}";
        case Value::K::range: return "{range}";
        case Value::K::thread: return "{thread}";
        case Value::K::mutex: return "{mutex}";
        case Value::K::channel: return "{channel}";
        case Value::K::atomic: return "{atomic}";
        case Value::K::box: return "{box}";
        case Value::K::arena: return "{arena}";
        case Value::K::shared: return "{shared}";
        case Value::K::weak: return "{weak}";
        case Value::K::rawptr: return "{raw pointer}";
        case Value::K::simd4f32: return "{simd4f32}";
        case Value::K::fixed_array: {
            std::string out = "[";
            for (size_t i = 0; i < v.array.size(); i++) {
                if (i) out += ", ";
                out += display(v.array[i]);
            }
            return out + "]";
        }
        case Value::K::slice: return "{slice}";
    }
    return "?";
}

Value Interp::coerce_arg(Value v, const TypeRef*) { return v; }

} // namespace beans
