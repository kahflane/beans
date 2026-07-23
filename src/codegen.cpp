#include "codegen.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include "builtins.h"
#include "c_abi.h"
#include "lexer.h"
#include "parser.h"
#include "value.h" // for Decimal::parse at compile time

namespace beans {

namespace {

std::string u128_str(unsigned __int128 v) {
    std::string s;
    if (v == 0) s = "0";
    while (v > 0) {
        s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(v % 10)));
        v /= 10;
    }
    return s;
}

// Native decimals are one LLVM i128 in locals, fields, parameters and returns.
// The low 112 bits hold a signed coefficient; the high 16 bits hold the scale.
// That keeps more precision than the promised ~28 digits while also preserving
// high-scale values such as 1e-40 without a heap object.
std::string packed_decimal(Decimal d) {
    constexpr int coeff_bits = 112;
    const unsigned __int128 coeff_mask =
        (static_cast<unsigned __int128>(1) << coeff_bits) - 1;
    unsigned __int128 bits = static_cast<unsigned __int128>(d.coeff) & coeff_mask;
    bits |= static_cast<unsigned __int128>(static_cast<uint16_t>(d.scale)) <<
            coeff_bits;
    return u128_str(bits);
}

int64_t parse_int_text(std::string_view text) {
    std::string clean;
    for (char c : text) {
        if (c != '_') clean.push_back(c);
    }
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X'))
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 16));
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B'))
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 2));
    return static_cast<int64_t>(std::strtoull(clean.c_str(), nullptr, 10));
}

double parse_float_text(std::string_view text) {
    std::string clean;
    for (char c : text) {
        if (c != '_') clean.push_back(c);
    }
    return std::strtod(clean.c_str(), nullptr);
}

std::string fmt_double(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(bits));
    return buf;
}

std::string clean_number(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

struct StrPiece {
    std::string text;
    ExprPtr expr;
    FmtSpec spec;
};
std::vector<StrPiece> split_interp(std::string_view raw,
                                   std::vector<std::unique_ptr<std::string>>& srcs) {
    std::vector<StrPiece> parts;
    std::string_view body =
        raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : std::string_view{};
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
            if (!cur.empty()) {
                parts.push_back({cur, nullptr, {}});
                cur.clear();
            }
            std::string seg(body.substr(start, j - 1 - start));
            FmtSpec spec;
            std::string expr_text(split_fmt_spec(seg, spec, nullptr));
            srcs.push_back(std::make_unique<std::string>(std::move(expr_text)));
            Lexer lx(*srcs.back());
            Parser ps(lx.scan_all());
            parts.push_back({"", ps.parse_standalone_expr(), spec});
            i = j;
            continue;
        }
        cur += c;
        i += 1;
    }
    if (!cur.empty()) parts.push_back({cur, nullptr, {}});
    return parts;
}

} // namespace

// ---- semantic types for codegen ---------------------------------------------

struct Ty {
    enum K {
        i64_, f64_, i1_, str_, unit_, dec_, obj_, enum_, list_, bad_,
        map_,     // args = {K, V}
        fn_,      // args = params..., ret last
        thread_,  // args = {ret}
        mutex_,   // args = {inner}
        chan_,    // args = {elem}
        box_,     // args = {inner}; move-only in checked source
        arena_,   // args = {inner}; append-only slots, bulk clear/drop
        shared_,  // args = {inner}; explicit atomic shared control block
        weak_,    // args = {inner}; does not keep Shared's value alive
        rawptr_,  // args = {inner}; unmanaged, only usable in unsafe blocks
        simd4f32_, // inline four-lane f32 vector
        struct_,   // inline named user struct
        fixed_array_, // args = {element}; inline LLVM array
        slice_,    // args = {element}; inline {pointer, length} view
        atomic_,
        bytes_,
        file_,
        mmap_,
    };
    K k = K::bad_;
    uint8_t bits = 0;          // semantic width; slots are widened for now
    bool is_unsigned = false;  // integer compare/divide/print mode
    uint32_t array_len = 0;
    uint32_t layout_size = 0;
    uint32_t layout_align = 1;
    std::string name;          // obj: impl/iface name, enum_: enum name
    std::vector<Ty*> args;     // enum_ args; list_ elem in args[0]
    const ClassDecl* iface = nullptr; // obj typed as an interface

    Ty* fn_ret() const { return args.empty() ? nullptr : args.back(); }
    size_t fn_nparams() const { return args.empty() ? 0 : args.size() - 1; }
};

namespace {
bool is_inline_option(const Ty* t);
bool is_inline_result(const Ty* t);

bool is_wide_inline_value(const Ty* t) {
    if (!t) return false;
    switch (t->k) {
        case Ty::simd4f32_:
        case Ty::struct_:
        case Ty::fixed_array_:
        case Ty::slice_:
            return true;
        case Ty::enum_:
            return is_inline_option(t) || is_inline_result(t);
        default:
            return false;
    }
}

bool is_inline_option(const Ty* t) {
    return t && t->k == Ty::enum_ && t->name == "Option" && t->args.size() == 1 &&
           is_wide_inline_value(t->args[0]);
}

bool is_inline_result(const Ty* t) {
    return t && t->k == Ty::enum_ && t->name == "Result" && t->args.size() == 2 &&
           (is_wide_inline_value(t->args[0]) || is_wide_inline_value(t->args[1]));
}

const char* ll(const Ty* t) {
    static thread_local std::string exact;
    switch (t->k) {
        case Ty::i64_:
            exact = "i" + std::to_string(t->bits ? t->bits : 64);
            return exact.c_str();
        case Ty::f64_: return t->bits == 32 ? "float" : "double";
        case Ty::i1_: return "i1";
        case Ty::dec_: return "i128";
        case Ty::simd4f32_: return "<4 x float>";
        case Ty::struct_:
            exact = "%bs." + t->name;
            return exact.c_str();
        case Ty::fixed_array_: {
            std::string element = ll(t->args[0]);
            exact = "[" + std::to_string(t->array_len) + " x " + element + "]";
            return exact.c_str();
        }
        case Ty::slice_: return "{ptr, i64}";
        case Ty::enum_:
            if (is_inline_option(t)) {
                std::string payload = ll(t->args[0]);
                exact = "{i1, " + payload + "}";
                return exact.c_str();
            }
            if (is_inline_result(t)) {
                std::string ok = ll(t->args[0]);
                std::string error = ll(t->args[1]);
                exact = "{i1, " + ok + ", " + error + "}";
                return exact.c_str();
            }
            return "ptr";
        case Ty::unit_: return "void";
        default: return "ptr";
    }
}
// does this type live on the RC'd heap?
bool is_rc(const Ty* t) {
    switch (t->k) {
        case Ty::str_: case Ty::obj_:
        case Ty::list_: case Ty::map_: case Ty::fn_: case Ty::thread_:
        case Ty::mutex_: case Ty::chan_: case Ty::box_: case Ty::arena_:
        case Ty::shared_: case Ty::weak_:
        case Ty::atomic_: case Ty::bytes_:
        case Ty::file_: case Ty::mmap_:
            return true;
        case Ty::enum_:
            return !is_inline_option(t) && !is_inline_result(t);
        default:
            return false;
    }
}
// Inline aggregates can own ARC pointers even though the aggregate itself is
// not an ARC allocation. Result is the first such type: its inactive field is
// always zero, so walking both payloads is safe and needs no tag branch.
bool has_owned_refs(const Ty* t) {
    if (!t) return false;
    if (is_rc(t)) return true;
    if (is_inline_option(t)) return has_owned_refs(t->args[0]);
    if (is_inline_result(t))
        return has_owned_refs(t->args[0]) || has_owned_refs(t->args[1]);
    return false;
}
// Generic runtime containers still use one i64 slot. Wider inline values are
// boxed only at that boundary, so their slot is a pointer even though the
// value itself is not reference counted in normal code.
bool is_slot_rc(const Ty* t) {
    return is_rc(t) || (t && t->k == Ty::dec_);
}
// Pointer values cannot be null in safe Beans code. Option can therefore use
// null for `none` and the value pointer itself for `some`, with no enum box.
// Keep nested/user enums boxed: they need their own tag space.
bool is_niche_option(const Ty* t) {
    return t && t->k == Ty::enum_ && t->name == "Option" && t->args.size() == 1 &&
           t->args[0]->k != Ty::enum_ && is_rc(t->args[0]);
}
} // namespace

// a monomorphized class
struct CImpl {
    const ClassDecl* decl = nullptr;
    std::map<std::string, Ty*> env;
    std::string mangled;
    int id = 0;
    CImpl* parent = nullptr;
    int size = 8; // payload bytes, starting with one static type descriptor
    int align = 8;
    struct FieldInfo {
        std::string name;
        Ty* ty;
        const FieldDecl* decl;
        int offset;
    };
    std::vector<FieldInfo> fields;
};

// ---- codegen implementation --------------------------------------------------

struct CG2 {
    std::vector<CGError>& errors;
    const HirProgram& hir;

    std::vector<std::unique_ptr<Ty>> ty_pool;
    std::map<std::string, Ty*> ty_map;
    std::vector<std::unique_ptr<CImpl>> impls;
    std::vector<std::unique_ptr<CImpl>> struct_impls;
    std::map<std::string, CImpl*> impl_by_name;
    std::vector<CImpl*> impl_queue;

    // keyed by package-qualified names ("util.User"; root plain)
    std::map<std::string, const ClassDecl*> class_decls; // classes + interfaces
    std::map<std::string, const EnumDecl*> enum_decls;
    std::map<std::string, const FnDecl*> fn_decls;

    // package the code being emitted lives in; resolves plain names that the
    // checker never annotated (string-interpolation segments)
    std::string cur_pkg;
    std::map<std::string, std::map<std::string, std::string>> pkg_imports;
    std::map<std::string, std::string> prefix_by_path;

    std::map<std::string, int> selectors;
    std::string globals;
    std::string type_defs;
    std::string fn_text;
    std::string ffi_c;
    std::map<const FnDecl*, std::string> extern_symbols;
    std::map<Ty*, std::string> callback_dispatches;
    std::map<const FnDecl*, std::string> fn_ref_adapters;
    int next_str = 0;

    explicit CG2(const HirProgram& hir, std::vector<CGError>& errs)
        : errors(errs), hir(hir) {
        const Program& prog = hir.ast();
        for (const auto& pkg : prog.packages) {
            prefix_by_path[pkg->import_path] = pkg->prefix;
            auto& bindings = pkg_imports[pkg->prefix];
            for (const auto& pf : pkg->files) {
                for (const ClassDecl& c : pf->mod.classes) class_decls[c.qualname] = &c;
                for (const EnumDecl& e : pf->mod.enums) enum_decls[e.qualname] = &e;
                for (const FnDecl& f : pf->mod.fns) fn_decls[f.qualname] = &f;
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

    std::string qual(const std::string& n) const {
        return cur_pkg.empty() ? n : cur_pkg + "." + n;
    }
    static std::string pkg_of(const std::string& qualname) {
        size_t dot = qualname.find('.');
        return dot == std::string::npos ? "" : qualname.substr(0, dot);
    }
    static const std::vector<std::string>& supers_of(const ClassDecl* c) {
        return c->supers_resolved.empty() ? c->supers : c->supers_resolved;
    }
    std::string binding_path(const std::string& binding) const {
        auto pit = pkg_imports.find(cur_pkg);
        if (pit == pkg_imports.end()) return "";
        auto it = pit->second.find(binding);
        return it == pit->second.end() ? "" : it->second;
    }

    void err(uint32_t line, uint32_t col, const std::string& msg) {
        errors.push_back({msg + " — not in the native backend yet (beansc run still works)",
                          line, col});
    }

    // ---- type interning ----
    Ty* intern(Ty t) {
        std::string key = std::to_string(t.k) + ":" + t.name + ":" +
                          std::to_string(t.bits) + ":" +
                          std::to_string(t.is_unsigned) + ":" +
                          std::to_string(t.array_len);
        for (Ty* a : t.args) key += "," + std::to_string(reinterpret_cast<uintptr_t>(a));
        auto it = ty_map.find(key);
        if (it != ty_map.end()) return it->second;
        ty_pool.push_back(std::make_unique<Ty>(std::move(t)));
        ty_map[key] = ty_pool.back().get();
        return ty_pool.back().get();
    }
    Ty* prim(Ty::K k) { Ty t; t.k = k; return intern(std::move(t)); }
    Ty* t_int(uint8_t bits, bool is_unsigned) {
        Ty t;
        t.k = Ty::i64_;
        t.bits = bits;
        t.is_unsigned = is_unsigned;
        return intern(std::move(t));
    }
    Ty* t_float(uint8_t bits) {
        Ty t;
        t.k = Ty::f64_;
        t.bits = bits;
        return intern(std::move(t));
    }
    Ty* t_i64() { return t_int(64, false); }
    Ty* t_f64() { return t_float(64); }
    Ty* t_bool() { return prim(Ty::i1_); }
    Ty* t_str() { return prim(Ty::str_); }
    Ty* t_unit() { return prim(Ty::unit_); }
    Ty* t_dec() { return prim(Ty::dec_); }
    Ty* t_bad() { return prim(Ty::bad_); }
    Ty* t_list(Ty* e) { Ty t; t.k = Ty::list_; t.args = {e}; return intern(std::move(t)); }
    Ty* t_enum(std::string n, std::vector<Ty*> a) {
        Ty t; t.k = Ty::enum_; t.name = std::move(n); t.args = std::move(a);
        return intern(std::move(t));
    }
    Ty* t_option(Ty* inner) { return t_enum("Option", {inner}); }
    Ty* t_error() { Ty t; t.k = Ty::obj_; t.name = "Error"; return intern(std::move(t)); }
    Ty* t_result(Ty* ok, Ty* e) { return t_enum("Result", {ok, e}); }
    Ty* t_obj(std::string n, const ClassDecl* iface = nullptr) {
        Ty t; t.k = Ty::obj_; t.name = std::move(n); t.iface = iface;
        return intern(std::move(t));
    }
    Ty* t_struct(std::string n, uint32_t size, uint32_t align) {
        Ty t;
        t.k = Ty::struct_;
        t.name = std::move(n);
        t.layout_size = size;
        t.layout_align = align;
        return intern(std::move(t));
    }
    Ty* t_kind1(Ty::K k, Ty* a) { Ty t; t.k = k; t.args = {a}; return intern(std::move(t)); }
    Ty* t_map(Ty* k, Ty* v, bool ordered = false) {
        Ty t;
        t.k = Ty::map_;
        t.name = ordered ? "OrderedMap" : "Map";
        t.args = {k, v};
        return intern(std::move(t));
    }
    Ty* t_box(Ty* inner) { return t_kind1(Ty::box_, inner); }
    Ty* t_arena(Ty* inner) { return t_kind1(Ty::arena_, inner); }
    Ty* t_shared(Ty* inner) { return t_kind1(Ty::shared_, inner); }
    Ty* t_weak(Ty* inner) { return t_kind1(Ty::weak_, inner); }
    Ty* t_rawptr(Ty* inner) { return t_kind1(Ty::rawptr_, inner); }
    Ty* t_simd4f32() { return prim(Ty::simd4f32_); }
    Ty* t_fixed_array(Ty* element, uint32_t length) {
        Ty t;
        t.k = Ty::fixed_array_;
        t.args = {element};
        t.array_len = length;
        return intern(std::move(t));
    }
    Ty* t_slice(Ty* element) { return t_kind1(Ty::slice_, element); }
    Ty* t_fn(std::vector<Ty*> params_then_ret) {
        Ty t; t.k = Ty::fn_; t.args = std::move(params_then_ret);
        return intern(std::move(t));
    }
    Ty* t_atomic() { return prim(Ty::atomic_); }
    Ty* t_bytes() { return prim(Ty::bytes_); }
    Ty* t_file() { return prim(Ty::file_); }
    Ty* t_mmap() { return prim(Ty::mmap_); }

    static int value_size(Ty* type) {
        if (type->k == Ty::i1_) return 1;
        if (type->k == Ty::i64_) return (type->bits ? type->bits : 64) / 8;
        if (type->k == Ty::f64_) return type->bits == 32 ? 4 : 8;
        if (type->k == Ty::dec_) return 16;
        if (type->k == Ty::simd4f32_) return 16;
        if (type->k == Ty::struct_) return static_cast<int>(type->layout_size);
        if (type->k == Ty::fixed_array_)
            return value_size(type->args[0]) * static_cast<int>(type->array_len);
        if (type->k == Ty::slice_) return 16;
        if (is_inline_option(type)) {
            int payload_align = value_align(type->args[0]);
            int payload_offset = align_up(1, payload_align);
            return align_up(payload_offset + value_size(type->args[0]), payload_align);
        }
        if (is_inline_result(type)) {
            int ok_align = value_align(type->args[0]);
            int error_align = value_align(type->args[1]);
            int aggregate_align = std::max(ok_align, error_align);
            int ok_offset = align_up(1, ok_align);
            int error_offset = align_up(ok_offset + value_size(type->args[0]), error_align);
            return align_up(error_offset + value_size(type->args[1]), aggregate_align);
        }
        if (type->k == Ty::unit_) return 0;
        return 8;
    }
    static int value_align(Ty* type) {
        if (type->k == Ty::dec_) return 16;
        if (type->k == Ty::simd4f32_) return 16;
        if (type->k == Ty::struct_) return static_cast<int>(type->layout_align);
        if (type->k == Ty::fixed_array_) return value_align(type->args[0]);
        if (type->k == Ty::slice_) return 8;
        if (is_inline_option(type)) return value_align(type->args[0]);
        if (is_inline_result(type))
            return std::max(value_align(type->args[0]), value_align(type->args[1]));
        int size = value_size(type);
        return size > 8 ? 8 : size > 0 ? size : 1;
    }
    static int align_up(int value, int align) {
        return (value + align - 1) & -align;
    }

    TypeId checked_type(const Expr* expr) const { return hir.type_of(expr); }
    Ty* checked_primitive(const Expr* expr) {
        TypeId type = checked_type(expr);
        if (!type) return nullptr;
        IntLayout integer = hir.target().integer(type->k);
        if (integer.bits) return t_int(integer.bits, !integer.is_signed);
        uint8_t floating = hir.target().float_bits(type->k);
        if (floating) return t_float(floating);
        if (type->k == Type::K::decimal_) return t_dec();
        return nullptr;
    }

    std::string mangle(Ty* t) {
        switch (t->k) {
            case Ty::i64_:
                return std::string(t->is_unsigned ? "u" : "i") +
                       std::to_string(t->bits ? t->bits : 64);
            case Ty::f64_: return "f" + std::to_string(t->bits ? t->bits : 64);
            case Ty::i1_: return "bool";
            case Ty::str_: return "string";
            case Ty::dec_: return "decimal";
            case Ty::obj_: return t->name;
            case Ty::struct_: return t->name;
            case Ty::enum_: {
                std::string name = t->name;
                for (Ty* arg : t->args) name += "_" + mangle(arg);
                return name;
            }
            case Ty::list_: return "List_" + mangle(t->args[0]);
            case Ty::map_: return "Map_" + mangle(t->args[0]) + "_" + mangle(t->args[1]);
            case Ty::box_: return "Box_" + mangle(t->args[0]);
            case Ty::arena_: return "Arena_" + mangle(t->args[0]);
            case Ty::shared_: return "Shared_" + mangle(t->args[0]);
            case Ty::weak_: return "Weak_" + mangle(t->args[0]);
            case Ty::rawptr_: return "RawPtr_" + mangle(t->args[0]);
            case Ty::simd4f32_: return "Simd4f32";
            case Ty::fixed_array_:
                return "Array" + std::to_string(t->array_len) + "_" +
                       mangle(t->args[0]);
            case Ty::slice_: return "Slice_" + mangle(t->args[0]);
            default: return "x";
        }
    }

    // ---- class instantiation ----
    CImpl* request_impl(const ClassDecl* decl, std::vector<Ty*> targs,
                        uint32_t line, uint32_t col) {
        std::string mangled = decl->qualname;
        for (Ty* a : targs) mangled += "$" + mangle(a);
        auto it = impl_by_name.find(mangled);
        if (it != impl_by_name.end()) return it->second;

        auto up = std::make_unique<CImpl>();
        CImpl* im = up.get();
        im->decl = decl;
        im->mangled = mangled;
        bool inline_value = decl->is_struct || decl->is_union;
        im->id = inline_value ? -1 : static_cast<int>(impls.size());
        if (inline_value) {
            im->size = 0;
            im->align = 1;
        }
        for (size_t i = 0; i < decl->generics.size() && i < targs.size(); i++) {
            im->env[decl->generics[i].name] = targs[i];
        }
        impl_by_name[mangled] = im;
        if (inline_value) {
            struct_impls.push_back(std::move(up));
        } else {
            impls.push_back(std::move(up));
            impl_queue.push_back(im);
        }

        // field types resolve as the class's own code
        std::string saved_pkg = cur_pkg;
        cur_pkg = pkg_of(decl->qualname);

        // parent chain (single class parent; interfaces carry no fields)
        if (!inline_value) {
            for (const std::string& s : supers_of(decl)) {
                auto cit = class_decls.find(s);
                if (cit != class_decls.end() && !cit->second->is_interface &&
                    !cit->second->is_struct && !cit->second->is_union) {
                    im->parent = request_impl(cit->second, {}, line, col);
                }
            }
        }
        // fields: parent's first, then own
        if (im->parent) {
            im->fields = im->parent->fields;
            im->size = im->parent->size;
        }
        for (const FieldDecl& f : decl->fields) {
            CImpl::FieldInfo fi;
            fi.name = f.name;
            fi.decl = &f;
            fi.ty = resolve(f.type.get(), im->env, f.line, f.col);
            fi.offset = decl->is_union ? 0 : align_up(im->size, value_align(fi.ty));
            im->size = decl->is_union
                           ? std::max(im->size, value_size(fi.ty))
                           : fi.offset + value_size(fi.ty);
            im->align = std::max(im->align, value_align(fi.ty));
            if (!inline_value && fi.offset / 8 > 57) {
                // pointer-slot masks stop at meta bit 60 — collector owns 61-63
                err(f.line, f.col, "class '" + decl->name + "' has too many fields");
            }
            im->fields.push_back(std::move(fi));
        }
        if (inline_value) {
            im->size = align_up(im->size, im->align);
            type_defs += "%bs." + im->mangled + " = type {";
            if (decl->is_union) {
                // The biggest member is not always the most aligned one:
                // `union { u8 bytes[16]; u64 word; }` is 16 bytes aligned to
                // 8 in C. Start with the most-aligned member, then pad to the
                // already-computed union size so LLVM gets both facts right.
                const CImpl::FieldInfo* storage = nullptr;
                for (const CImpl::FieldInfo& field : im->fields) {
                    if (!storage || value_align(field.ty) > value_align(storage->ty) ||
                        (value_align(field.ty) == value_align(storage->ty) &&
                         value_size(field.ty) > value_size(storage->ty)))
                        storage = &field;
                }
                if (storage) {
                    type_defs += ll(storage->ty);
                    int padding = im->size - value_size(storage->ty);
                    if (padding > 0)
                        type_defs += ", [" + std::to_string(padding) + " x i8]";
                } else {
                    type_defs += "i8";
                }
            } else {
                for (size_t i = 0; i < im->fields.size(); i++) {
                    if (i) type_defs += ", ";
                    type_defs += ll(im->fields[i].ty);
                }
            }
            type_defs += "}\n";
        }
        cur_pkg = saved_pkg;
        return im;
    }

    // ---- type resolution ----
    Ty* resolve(const TypeRef* t, const std::map<std::string, Ty*>& env,
                uint32_t line, uint32_t col) {
        if (!t) return t_unit();
        if (t->kind == TypeRef::Kind::fixed_array) {
            return t_fixed_array(resolve(t->array_elem.get(), env, line, col),
                                 t->array_len);
        }
        if (t->kind == TypeRef::Kind::fn) {
            std::vector<Ty*> sig;
            for (const TypePtr& p : t->fn_params)
                sig.push_back(resolve(p.get(), env, line, col));
            sig.push_back(t->fn_ret ? resolve(t->fn_ret.get(), env, line, col) : t_unit());
            return t_fn(std::move(sig));
        }
        const std::string& n = t->name;
        auto eit = env.find(n);
        if (eit != env.end()) return eit->second;

        if (n == "int" || n == "i64") return t_int(64, false);
        if (n == "i8") return t_int(8, false);
        if (n == "i16") return t_int(16, false);
        if (n == "i32") return t_int(32, false);
        if (n == "u8" || n == "byte") return t_int(8, true);
        if (n == "u16") return t_int(16, true);
        if (n == "u32") return t_int(32, true);
        if (n == "u64") return t_int(64, true);
        if (n == "float" || n == "f64") return t_float(64);
        if (n == "f32") return t_float(32);
        if (n == "bool") return t_bool();
        if (n == "string") return t_str();
        if (n == "decimal") return t_dec();
        if (n == "Error") return t_error();
        if (n == "List" && t->args.size() == 1)
            return t_list(resolve(t->args[0].get(), env, line, col));
        if ((n == "Map" || n == "OrderedMap") && t->args.size() == 2)
            return t_map(resolve(t->args[0].get(), env, line, col),
                         resolve(t->args[1].get(), env, line, col),
                         n == "OrderedMap");
        if (n == "Thread" && t->args.size() == 1)
            return t_kind1(Ty::thread_, resolve(t->args[0].get(), env, line, col));
        if (n == "Mutex" && t->args.size() == 1)
            return t_kind1(Ty::mutex_, resolve(t->args[0].get(), env, line, col));
        if (n == "Channel" && t->args.size() == 1)
            return t_kind1(Ty::chan_, resolve(t->args[0].get(), env, line, col));
        if (n == "Box" && t->args.size() == 1)
            return t_box(resolve(t->args[0].get(), env, line, col));
        if (n == "Arena" && t->args.size() == 1)
            return t_arena(resolve(t->args[0].get(), env, line, col));
        if (n == "Shared" && t->args.size() == 1)
            return t_shared(resolve(t->args[0].get(), env, line, col));
        if (n == "Weak" && t->args.size() == 1)
            return t_weak(resolve(t->args[0].get(), env, line, col));
        if (n == "RawPtr" && t->args.size() == 1)
            return t_rawptr(resolve(t->args[0].get(), env, line, col));
        if (n == "Slice" && t->args.size() == 1)
            return t_slice(resolve(t->args[0].get(), env, line, col));
        if (n == "Simd4f32") return t_simd4f32();
        if (n == "AtomicInt") return t_atomic();
        if (n == "Bytes") return t_bytes();
        if (n == "File") return t_file();
        if (n == "MMap") return t_mmap();
        if (n == "Option" && t->args.size() == 1)
            return t_option(resolve(t->args[0].get(), env, line, col));
        if (n == "Result") {
            Ty* ok = t->args.empty() ? t_bad()
                                     : resolve(t->args[0].get(), env, line, col);
            Ty* e = t->args.size() >= 2 ? resolve(t->args[1].get(), env, line, col)
                                        : t_error();
            return t_result(ok, e);
        }
        // user types: the checker pinned cross-package names; plain names come
        // from unannotated code (interpolation segments) and mean cur_pkg
        const std::string& key = !t->resolved.empty() ? t->resolved : qual(n);
        auto cit = class_decls.find(key);
        if (cit != class_decls.end()) {
            if (cit->second->is_interface) return t_obj(key, cit->second);
            std::vector<Ty*> targs;
            for (const TypePtr& a : t->args)
                targs.push_back(resolve(a.get(), env, line, col));
            CImpl* im = request_impl(cit->second, std::move(targs), line, col);
            return (cit->second->is_struct || cit->second->is_union)
                       ? t_struct(im->mangled, static_cast<uint32_t>(im->size),
                                  static_cast<uint32_t>(im->align))
                       : t_obj(im->mangled);
        }
        auto enit = enum_decls.find(key);
        if (enit != enum_decls.end()) {
            if (!t->args.empty()) {
                err(line, col, "generic enums");
                return t_bad();
            }
            return t_enum(key, {});
        }
        err(line, col, "type '" + n + "'");
        return t_bad();
    }

    const ClassDecl* c_abi_record(const TypeRef* type) const {
        if (!type || type->kind != TypeRef::Kind::named) return nullptr;
        std::string key = type->resolved.empty() ? type->name : type->resolved;
        auto found = class_decls.find(key);
        if (found == class_decls.end() && type->resolved.empty())
            found = class_decls.find(qual(type->name));
        if (found == class_decls.end() || !found->second->is_c_layout ||
            (!found->second->is_struct && !found->second->is_union))
            return nullptr;
        return found->second;
    }

    bool extern_has_aggregate(const FnDecl* function) const {
        if (!function || !function->is_extern_c) return false;
        if (function->ret && c_abi_record(function->ret.get())) return true;
        for (const Param& parameter : function->params) {
            if (parameter.type && parameter.type->kind == TypeRef::Kind::fn)
                return true;
            if (c_abi_record(parameter.type.get())) return true;
        }
        return false;
    }

    std::string request_callback_dispatch(Ty* function_type) {
        auto found = callback_dispatches.find(function_type);
        if (found != callback_dispatches.end()) return found->second;
        std::string name = "beans_cb_dispatch_" +
                           std::to_string(callback_dispatches.size());
        std::string symbol = "@" + name;
        callback_dispatches[function_type] = symbol;
        std::string body = "define void " + symbol +
                           "(ptr %closure, ptr %result, ptr %args) {\n"
                           "  %fn = load ptr, ptr %closure\n";
        std::vector<std::string> values;
        for (size_t i = 0; i < function_type->fn_nparams(); i++) {
            std::string slot = "%as" + std::to_string(i);
            std::string pointer = "%ap" + std::to_string(i);
            std::string value = "%av" + std::to_string(i);
            body += "  " + slot + " = getelementptr ptr, ptr %args, i64 " +
                    std::to_string(i) + "\n";
            body += "  " + pointer + " = load ptr, ptr " + slot + "\n";
            body += "  " + value + " = load " +
                    std::string(ll(function_type->args[i])) + ", ptr " + pointer + "\n";
            values.push_back(value);
        }
        Ty* result = function_type->fn_ret();
        std::string call_args = "ptr %closure";
        for (size_t i = 0; i < values.size(); i++)
            call_args += ", " + std::string(ll(function_type->args[i])) + " " + values[i];
        if (!result || result->k == Ty::unit_) {
            body += "  call void %fn(" + call_args + ")\n  ret void\n}\n\n";
        } else {
            body += "  %return = call " + std::string(ll(result)) + " %fn(" +
                    call_args + ")\n";
            body += "  store " + std::string(ll(result)) +
                    " %return, ptr %result\n  ret void\n}\n\n";
        }
        lifted += body;
        return symbol;
    }

    std::string request_extern(const FnDecl* function) {
        auto existing = extern_symbols.find(function);
        if (existing != extern_symbols.end()) return existing->second;

        if (!extern_has_aggregate(function)) {
            Ty* ret = resolve(function->ret.get(), empty_env(), function->line,
                              function->col);
            std::string symbol = "@" + function->extern_name;
            std::string declaration = "declare " + std::string(ll(ret)) + " " +
                                      symbol + "(";
            for (size_t i = 0; i < function->params.size(); i++) {
                if (i) declaration += ", ";
                Ty* parameter = resolve(function->params[i].type.get(), empty_env(),
                                        function->params[i].line,
                                        function->params[i].col);
                declaration += ll(parameter);
            }
            globals += declaration + ")\n";
            extern_symbols[function] = symbol;
            return symbol;
        }

        std::string wrapper = "beans_ffi_wrap_" +
                              std::to_string(extern_symbols.size());
        globals += "declare void @" + wrapper + "(ptr, ptr)\n";
        if (ffi_c.empty()) ffi_c = "#include <stdint.h>\n";
        CAbiText abi = describe_c_abi(
            *function, [&](const TypeRef* type) { return c_abi_record(type); }, wrapper);
        ffi_c += abi.definitions;
        for (const CAbiCallbackText& callback : abi.callbacks) {
            Ty* type = resolve(function->params[callback.parameter_index].type.get(),
                               empty_env(), function->line, function->col);
            std::string dispatch = request_callback_dispatch(type);
            if (!dispatch.empty() && dispatch[0] == '@') dispatch.erase(0, 1);
            std::string prefix = wrapper + "_cb" +
                                 std::to_string(callback.parameter_index);
            ffi_c += "static _Thread_local void* " + prefix + "_env;\n";
            ffi_c += "extern void " + dispatch + "(void*, void*, void**);\n";
            ffi_c += "static " + callback.return_type + " " + prefix + "(";
            for (size_t i = 0; i < callback.parameter_declarations.size(); i++) {
                if (i) ffi_c += ", ";
                ffi_c += callback.parameter_declarations[i];
            }
            if (callback.parameter_declarations.empty()) ffi_c += "void";
            ffi_c += ") {\n  void* callback_args[" +
                     std::to_string(std::max<size_t>(callback.parameter_types.size(), 1)) +
                     "] = {";
            for (size_t i = 0; i < callback.parameter_types.size(); i++) {
                if (i) ffi_c += ", ";
                ffi_c += "&value" + std::to_string(i);
            }
            if (callback.parameter_types.empty()) ffi_c += "0";
            ffi_c += "};\n";
            if (callback.return_type == "void") {
                ffi_c += "  " + dispatch + "(" + prefix +
                         "_env, 0, callback_args);\n}\n";
            } else {
                ffi_c += "  " + callback.return_type + " callback_result;\n  " +
                         dispatch + "(" + prefix +
                         "_env, &callback_result, callback_args);\n"
                         "  return callback_result;\n}\n";
            }
        }
        ffi_c += "\nvoid " + wrapper + "(void* result, void** args) {\n";
        ffi_c += "  extern " + abi.return_type + " " + function->extern_name + "(";
        for (size_t i = 0; i < abi.parameter_declarations.size(); i++) {
            if (i) ffi_c += ", ";
            ffi_c += abi.parameter_declarations[i];
        }
        if (abi.parameter_declarations.empty()) ffi_c += "void";
        ffi_c += ");\n";
        for (const CAbiCallbackText& callback : abi.callbacks) {
            std::string prefix = wrapper + "_cb" +
                                 std::to_string(callback.parameter_index);
            ffi_c += "  void* " + prefix + "_old = " + prefix + "_env;\n";
            ffi_c += "  " + prefix + "_env = *(void**)args[" +
                     std::to_string(callback.parameter_index) + "];\n";
        }
        ffi_c += "  ";
        if (function->ret) ffi_c += abi.return_type + " call_result = ";
        ffi_c += function->extern_name + "(";
        for (size_t i = 0; i < abi.parameter_types.size(); i++) {
            if (i) ffi_c += ", ";
            auto callback = std::find_if(
                abi.callbacks.begin(), abi.callbacks.end(),
                [&](const CAbiCallbackText& value) { return value.parameter_index == i; });
            if (callback != abi.callbacks.end())
                ffi_c += wrapper + "_cb" + std::to_string(i);
            else
                ffi_c += "*(" + abi.parameter_types[i] + "*)args[" +
                         std::to_string(i) + "]";
        }
        ffi_c += ");\n";
        for (const CAbiCallbackText& callback : abi.callbacks) {
            std::string prefix = wrapper + "_cb" +
                                 std::to_string(callback.parameter_index);
            ffi_c += "  " + prefix + "_env = " + prefix + "_old;\n";
        }
        if (function->ret)
            ffi_c += "  *(" + abi.return_type + "*)result = call_result;\n";
        ffi_c += "}\n";
        std::string symbol = "@" + wrapper;
        extern_symbols[function] = symbol;
        return symbol;
    }

    // ---- misc lookups ----
    // string constants carry an immortal RC header so releases skip them;
    // the value handed around points 16 bytes past the header
    std::string intern_string(const std::string& bytes) {
        std::string name = "@.str" + std::to_string(next_str++);
        // meta shape bits carry the byte length, same as heap strings
        globals += name + " = private unnamed_addr constant {i64, i64, [" +
                   std::to_string(bytes.size() + 1) +
                   " x i8]} {i64 4611686018427387904, i64 " +
                   std::to_string(static_cast<long long>(bytes.size()) << 3) + ", [" +
                   std::to_string(bytes.size() + 1) + " x i8] c\"";
        for (unsigned char c : bytes) {
            if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') {
                globals += static_cast<char>(c);
            } else {
                char buf[4];
                std::snprintf(buf, sizeof buf, "\\%02X", c);
                globals += buf;
            }
        }
        globals += "\\00\"}\n";
        return "getelementptr (i8, ptr " + name + ", i64 16)";
    }

    // payload-less enum values (none, error tags, bare variants) are
    // immutable and carry only their tag: one immortal box per tag number,
    // shared program-wide like string constants. Spares tree-shaped data
    // an allocation and a release per empty child.
    std::map<int, std::string> enum_tag_consts;
    std::string intern_enum_tag(int tag) {
        auto it = enum_tag_consts.find(tag);
        if (it != enum_tag_consts.end()) return it->second;
        std::string name = "@.etag" + std::to_string(tag);
        globals += name + " = private unnamed_addr constant {i64, i64, i64} "
                   "{i64 4611686018427387904, i64 1, i64 " + // immortal, kind 1
                   std::to_string(tag) + "}\n";
        std::string ref = "getelementptr (i8, ptr " + name + ", i64 16)";
        enum_tag_consts[tag] = ref;
        return ref;
    }

    int selector(const std::string& name) {
        auto it = selectors.find(name);
        if (it != selectors.end()) return it->second;
        int id = static_cast<int>(selectors.size());
        selectors[name] = id;
        return id;
    }

    // find a method decl (with its env) starting from a class impl,
    // walking parent classes then interfaces (for signatures and defaults)
    struct FoundMethod {
        const FnDecl* decl = nullptr;
        const std::map<std::string, Ty*>* env = nullptr;
        std::string owner; // symbol owner: impl mangled name or interface name
        bool from_iface = false;
    };
    static const std::map<std::string, Ty*>& empty_env() {
        static const std::map<std::string, Ty*> e;
        return e;
    }
    FoundMethod find_method_class(CImpl* im, const std::string& name, bool want_body) {
        for (CImpl* c = im; c; c = c->parent) {
            for (const FnDecl& m : c->decl->methods) {
                if (m.name == name && m.has_self && (!want_body || m.has_body)) {
                    return {&m, &c->env, c->mangled, false};
                }
            }
        }
        // interfaces anywhere in the chain
        std::vector<const ClassDecl*> work;
        for (CImpl* c = im; c; c = c->parent) {
            for (const std::string& s : supers_of(c->decl)) {
                auto cit = class_decls.find(s);
                if (cit != class_decls.end() && cit->second->is_interface)
                    work.push_back(cit->second);
            }
        }
        while (!work.empty()) {
            const ClassDecl* ic = work.back();
            work.pop_back();
            for (const FnDecl& m : ic->methods) {
                if (m.name == name && m.has_self && (!want_body || m.has_body)) {
                    return {&m, &empty_env(), ic->qualname, true};
                }
            }
            for (const std::string& s : supers_of(ic)) {
                auto cit = class_decls.find(s);
                if (cit != class_decls.end()) work.push_back(cit->second);
            }
        }
        return {};
    }
    FoundMethod find_method_iface(const ClassDecl* iface, const std::string& name) {
        std::vector<const ClassDecl*> work = {iface};
        while (!work.empty()) {
            const ClassDecl* ic = work.back();
            work.pop_back();
            for (const FnDecl& m : ic->methods) {
                if (m.name == name && m.has_self) return {&m, &empty_env(), ic->qualname, true};
            }
            for (const std::string& s : supers_of(ic)) {
                auto cit = class_decls.find(s);
                if (cit != class_decls.end()) work.push_back(cit->second);
            }
        }
        return {};
    }

    int variant_tag(const std::string& enum_name, const std::string& variant) {
        if (enum_name == "Option") return variant == "some" ? 0 : 1;
        if (enum_name == "Result") return variant == "ok" ? 0 : 1;
        auto it = enum_decls.find(enum_name);
        if (it == enum_decls.end()) return -1;
        int i = 0;
        for (const EnumVariant& v : it->second->variants) {
            if (v.name == variant) return i;
            i++;
        }
        return -1;
    }
    const EnumVariant* variant_decl(const std::string& enum_name,
                                    const std::string& variant) {
        auto it = enum_decls.find(enum_name);
        if (it == enum_decls.end()) return nullptr;
        for (const EnumVariant& v : it->second->variants) {
            if (v.name == variant) return &v;
        }
        return nullptr;
    }

    // lifted closure fns + monomorphized generic fns, appended at the end
    std::string lifted;
    int next_clo = 0;

    // sort_by bridges: the C merge sort only knows (box, slot, slot) -> i64,
    // so each element type gets one thunk that untypes the slots, calls the
    // fn value's code ptr (box layout {fnptr @0, ...}), and widens the i1
    std::map<Ty*, std::string> sort_thunks;
    std::map<Ty*, std::string> sort_key_thunks;
    std::string request_sort_thunk(Ty* elem) {
        auto it = sort_thunks.find(elem);
        if (it != sort_thunks.end()) return it->second;
        std::string sym = "@bsortcmp" + std::to_string(sort_thunks.size());
        sort_thunks[elem] = sym;
        std::string lt = ll(elem);
        std::string t = "define internal i64 " + sym + "(ptr %box, i64 %a, i64 %b) {\n";
        std::string ta = "%a", tb = "%b";
        if (elem->k == Ty::f64_) {
            if (elem->bits == 32) {
                t += "  %a32 = trunc i64 %a to i32\n  %b32 = trunc i64 %b to i32\n";
                t += "  %ta = bitcast i32 %a32 to float\n"
                     "  %tb = bitcast i32 %b32 to float\n";
            } else {
                t += "  %ta = bitcast i64 %a to double\n"
                     "  %tb = bitcast i64 %b to double\n";
            }
            ta = "%ta", tb = "%tb";
        } else if (elem->k == Ty::i1_) {
            t += "  %ta = trunc i64 %a to i1\n  %tb = trunc i64 %b to i1\n";
            ta = "%ta", tb = "%tb";
        } else if (elem->k == Ty::dec_) {
            t += "  %ap = inttoptr i64 %a to ptr\n"
                 "  %bp = inttoptr i64 %b to ptr\n"
                 "  %ta = call i128 @beans_decv_unbox(ptr %ap)\n"
                 "  %tb = call i128 @beans_decv_unbox(ptr %bp)\n";
            ta = "%ta", tb = "%tb";
        } else if (elem->k != Ty::i64_) {
            t += "  %ta = inttoptr i64 %a to ptr\n  %tb = inttoptr i64 %b to ptr\n";
            ta = "%ta", tb = "%tb";
        }
        t += "  %fp = load ptr, ptr %box\n";
        t += "  %r = call i1 %fp(ptr %box, " + lt + " " + ta + ", " + lt + " " + tb + ")\n";
        t += "  %z = zext i1 %r to i64\n  ret i64 %z\n}\n";
        lifted += t;
        return sym;
    }

    // sort_by_key bridges: evaluate one integer key per element, then let the
    // runtime's stable radix path reorder key/value pairs without more calls.
    std::string request_sort_key_thunk(Ty* elem) {
        auto it = sort_key_thunks.find(elem);
        if (it != sort_key_thunks.end()) return it->second;
        std::string sym = "@bsortkey" + std::to_string(sort_key_thunks.size());
        sort_key_thunks[elem] = sym;
        std::string lt = ll(elem);
        std::string t = "define internal i64 " + sym + "(ptr %box, i64 %a) {\n";
        std::string value = "%a";
        if (elem->k == Ty::f64_) {
            if (elem->bits == 32) {
                t += "  %a32 = trunc i64 %a to i32\n"
                     "  %ta = bitcast i32 %a32 to float\n";
            } else {
                t += "  %ta = bitcast i64 %a to double\n";
            }
            value = "%ta";
        } else if (elem->k == Ty::i1_) {
            t += "  %ta = trunc i64 %a to i1\n";
            value = "%ta";
        } else if (elem->k == Ty::dec_) {
            t += "  %ap = inttoptr i64 %a to ptr\n"
                 "  %ta = call i128 @beans_decv_unbox(ptr %ap)\n";
            value = "%ta";
        } else if (elem->k != Ty::i64_) {
            t += "  %ta = inttoptr i64 %a to ptr\n";
            value = "%ta";
        }
        t += "  %fp = load ptr, ptr %box\n";
        t += "  %r = call i64 %fp(ptr %box, " + lt + " " + value + ")\n";
        t += "  ret i64 %r\n}\n";
        lifted += t;
        return sym;
    }

    // display helpers: one `ptr @bshowN(i64 slot)` per shown type, returning
    // an owned string that matches the interpreter's display() exactly.
    // The symbol is memoized before the body is built so recursive types
    // (List<Tree> inside Tree) close over themselves.
    std::map<Ty*, std::string> show_fns;
    std::string request_show(Ty* t) {
        auto it = show_fns.find(t);
        if (it != show_fns.end()) return it->second;
        std::string sym = "@bshow" + std::to_string(show_fns.size());
        show_fns[t] = sym;
        std::string b = "define internal ptr " + sym + "(i64 %v) {\n";
        switch (t->k) {
            case Ty::i64_:
                b += "  %r = call ptr @beans_from_" +
                     std::string(t->is_unsigned ? "uint" : "int") +
                     "(i64 %v)\n  ret ptr %r\n";
                break;
            case Ty::f64_:
                if (t->bits == 32) {
                    b += "  %b = trunc i64 %v to i32\n"
                         "  %f = bitcast i32 %b to float\n"
                         "  %d = fpext float %f to double\n"
                         "  %r = call ptr @beans_from_float(double %d)\n  ret ptr %r\n";
                } else {
                    b += "  %f = bitcast i64 %v to double\n"
                         "  %r = call ptr @beans_from_float(double %f)\n  ret ptr %r\n";
                }
                break;
            case Ty::i1_:
                b += "  %t = trunc i64 %v to i1\n  %z = zext i1 %t to i32\n"
                     "  %r = call ptr @beans_from_bool(i32 %z)\n  ret ptr %r\n";
                break;
            case Ty::dec_:
                b += "  %p = inttoptr i64 %v to ptr\n"
                     "  %r = call ptr @beans_dec_str(ptr %p)\n  ret ptr %r\n";
                break;
            case Ty::str_:
                b += "  %p = inttoptr i64 %v to ptr\n"
                     "  call void @beans_retain(ptr %p)\n  ret ptr %p\n";
                break;
            case Ty::list_: {
                std::string es = request_show(t->args[0]);
                b += "  %p = inttoptr i64 %v to ptr\n"
                     "  %r = call ptr @beans_show_list(ptr %p, ptr " + es + ")\n"
                     "  ret ptr %r\n";
                break;
            }
            case Ty::enum_: {
                // enums can nest to data depth (Chain.link(Chain...)) — render
                // through the iterative driver so printing never recurses
                std::string st = request_step(t);
                b += "  %r = call ptr @beans_show_run(ptr " + st + ", i64 %v)\n"
                     "  ret ptr %r\n";
                break;
            }
            default:
                b += "  ret ptr " + intern_string("?") + "\n";
                break;
        }
        b += "}\n";
        lifted += b;
        return sym;
    }

    // iterative show steps: `void @bstepN(ptr ctx, i64 v)` appends this
    // value's own text and PUSHES child work onto the driver's stack instead
    // of calling other shows — beans_show_run drains it, so printing never
    // recurses on data depth. Output bytes match the interpreter's display()
    // exactly. Memoized before the body is built so recursive enums close
    // over themselves.
    std::map<Ty*, std::string> step_fns;
    std::string request_step(Ty* t) {
        auto it = step_fns.find(t);
        if (it != step_fns.end()) return it->second;
        std::string sym = "@bstep" + std::to_string(step_fns.size());
        step_fns[t] = sym;
        std::string b = "define internal void " + sym + "(ptr %c, i64 %v) {\n";
        switch (t->k) {
            case Ty::i64_:
                b += "  %s = call ptr @beans_from_" +
                     std::string(t->is_unsigned ? "uint" : "int") + "(i64 %v)\n" +
                     "  call void @beans_show_append(ptr %c, ptr %s)\n"
                     "  call void @beans_release(ptr %s)\n  ret void\n";
                break;
            case Ty::f64_:
                if (t->bits == 32) {
                    b += "  %b = trunc i64 %v to i32\n"
                         "  %f = bitcast i32 %b to float\n"
                         "  %d = fpext float %f to double\n"
                         "  %s = call ptr @beans_from_float(double %d)\n";
                } else {
                    b += "  %f = bitcast i64 %v to double\n"
                         "  %s = call ptr @beans_from_float(double %f)\n";
                }
                b +=
                     "  call void @beans_show_append(ptr %c, ptr %s)\n"
                     "  call void @beans_release(ptr %s)\n  ret void\n";
                break;
            case Ty::i1_:
                b += "  %t = trunc i64 %v to i1\n  %z = zext i1 %t to i32\n"
                     "  %s = call ptr @beans_from_bool(i32 %z)\n"
                     "  call void @beans_show_append(ptr %c, ptr %s)\n"
                     "  call void @beans_release(ptr %s)\n  ret void\n";
                break;
            case Ty::dec_:
                b += "  %p = inttoptr i64 %v to ptr\n"
                     "  %s = call ptr @beans_dec_str(ptr %p)\n"
                     "  call void @beans_show_append(ptr %c, ptr %s)\n"
                     "  call void @beans_release(ptr %s)\n  ret void\n";
                break;
            case Ty::str_:
                b += "  %p = inttoptr i64 %v to ptr\n"
                     "  call void @beans_show_append(ptr %c, ptr %p)\n  ret void\n";
                break;
            case Ty::list_: {
                std::string es = request_step(t->args[0]);
                b += "  %p = inttoptr i64 %v to ptr\n"
                     "  call void @beans_show_list_iter(ptr %c, ptr %p, ptr " + es +
                     ")\n  ret void\n";
                break;
            }
            case Ty::enum_: {
                if (is_niche_option(t)) {
                    std::string inner = request_step(t->args[0]);
                    b += "  %isnone = icmp eq i64 %v, 0\n"
                         "  br i1 %isnone, label %none, label %some\n"
                         "none:\n  call void @beans_show_append(ptr %c, ptr " +
                         intern_string("none") +
                         ")\n  ret void\n"
                         "some:\n  call void @beans_show_append(ptr %c, ptr " +
                         intern_string("some(") +
                         ")\n"
                         "  call void @beans_show_push_lit(ptr %c, ptr " +
                         intern_string(")") +
                         ")\n"
                         "  call void @beans_show_push_val(ptr %c, ptr " + inner +
                         ", i64 %v)\n  ret void\n";
                    break;
                }
                struct VarInfo {
                    std::string name;
                    std::vector<Ty*> pays;
                };
                std::vector<VarInfo> vars;
                if (t->name == "Option") {
                    vars.push_back({"some", {t->args.empty() ? nullptr : t->args[0]}});
                    vars.push_back({"none", {}});
                } else if (const EnumDecl* d = enum_decls.count(t->name)
                                                   ? enum_decls[t->name]
                                                   : nullptr) {
                    std::map<std::string, Ty*> env;
                    for (size_t i = 0; i < d->generics.size() && i < t->args.size(); i++) {
                        env[d->generics[i].name] = t->args[i];
                    }
                    for (const EnumVariant& v : d->variants) {
                        VarInfo vi{v.name, {}};
                        for (const Param& p : v.payload) {
                            vi.pays.push_back(resolve(p.type.get(), env, 0, 0));
                        }
                        vars.push_back(std::move(vi));
                    }
                }
                b += "  %e = inttoptr i64 %v to ptr\n  %tag = load i64, ptr %e\n";
                b += "  switch i64 %tag, label %vend [";
                for (size_t i = 0; i < vars.size(); i++) {
                    b += " i64 " + std::to_string(i) + ", label %v" + std::to_string(i);
                }
                b += " ]\n";
                int r = 0;
                for (size_t i = 0; i < vars.size(); i++) {
                    const VarInfo& vi = vars[i];
                    b += "v" + std::to_string(i) + ":\n";
                    if (vi.pays.empty() || !vi.pays[0]) {
                        b += "  call void @beans_show_append(ptr %c, ptr " +
                             intern_string(vi.name) + ")\n  ret void\n";
                        continue;
                    }
                    // append "name(" now; push ")" then payloads in reverse so
                    // they pop as p0, ", ", p1, ..., ")"
                    b += "  call void @beans_show_append(ptr %c, ptr " +
                         intern_string(vi.name + "(") + ")\n";
                    b += "  call void @beans_show_push_lit(ptr %c, ptr " +
                         intern_string(")") + ")\n";
                    for (size_t pi = vi.pays.size(); pi-- > 1;) {
                        std::string ps = request_step(vi.pays[pi]);
                        std::string pl = "%r" + std::to_string(r++);
                        std::string ld = "%r" + std::to_string(r++);
                        b += "  " + pl + " = getelementptr i8, ptr %e, i64 " +
                             std::to_string(8 + 8 * pi) + "\n";
                        b += "  " + ld + " = load i64, ptr " + pl + "\n";
                        b += "  call void @beans_show_push_val(ptr %c, ptr " + ps +
                             ", i64 " + ld + ")\n";
                        b += "  call void @beans_show_push_lit(ptr %c, ptr " +
                             intern_string(", ") + ")\n";
                    }
                    std::string ps0 = request_step(vi.pays[0]);
                    std::string pl0 = "%r" + std::to_string(r++);
                    std::string ld0 = "%r" + std::to_string(r++);
                    b += "  " + pl0 + " = getelementptr i8, ptr %e, i64 8\n";
                    b += "  " + ld0 + " = load i64, ptr " + pl0 + "\n";
                    b += "  call void @beans_show_push_val(ptr %c, ptr " + ps0 +
                         ", i64 " + ld0 + ")\n  ret void\n";
                }
                b += "vend:\n  call void @beans_show_append(ptr %c, ptr " +
                     intern_string("?") + ")\n  ret void\n";
                break;
            }
            default:
                b += "  call void @beans_show_append(ptr %c, ptr " +
                     intern_string("?") + ")\n  ret void\n";
                break;
        }
        b += "}\n";
        lifted += b;
        return sym;
    }

    // structural equality: one `i64 @beqN(i64 a, i64 b)` per type, returning
    // 0/1 and matching the interpreter's value_eq arm for arm — floats by IEEE
    // value (NaN equals nothing, -0.0 equals 0.0), decimals by value, strings
    // and Bytes by content, enums deep over payloads, lists/objects by
    // identity, maps and resource handles never equal (value_eq's default).
    // Plain i64 signature so the C runtime's list/map helpers can call through
    // a fn pointer; memoized before the body is built so recursive enums close
    // over themselves. No pointer-equality shortcut: some(nan) must not equal
    // itself, exactly like the interpreter.
    std::map<Ty*, std::string> eq_fns;
    std::string request_eq(Ty* t) {
        auto it = eq_fns.find(t);
        if (it != eq_fns.end()) return it->second;
        std::string sym = "@beq" + std::to_string(eq_fns.size());
        eq_fns[t] = sym;
        std::string b = "define internal i64 " + sym + "(i64 %a, i64 %b) {\n";
        switch (t->k) {
            case Ty::i64_:
            case Ty::i1_:
            case Ty::unit_:
            case Ty::list_:
            case Ty::obj_: // lists/objects: identity, like value_eq
                b += "  %c = icmp eq i64 %a, %b\n  %z = zext i1 %c to i64\n"
                     "  ret i64 %z\n";
                break;
            case Ty::f64_:
                if (t->bits == 32) {
                    b += "  %a32 = trunc i64 %a to i32\n"
                         "  %b32 = trunc i64 %b to i32\n"
                         "  %x = bitcast i32 %a32 to float\n"
                         "  %y = bitcast i32 %b32 to float\n"
                         "  %c = fcmp oeq float %x, %y\n";
                } else {
                    b += "  %x = bitcast i64 %a to double\n"
                         "  %y = bitcast i64 %b to double\n"
                         "  %c = fcmp oeq double %x, %y\n";
                }
                b += "  %z = zext i1 %c to i64\n  ret i64 %z\n";
                break;
            case Ty::str_:
                b += "  %p = inttoptr i64 %a to ptr\n  %q = inttoptr i64 %b to ptr\n"
                     "  %c = call i64 @beans_str_eq(ptr %p, ptr %q)\n  ret i64 %c\n";
                break;
            case Ty::dec_:
                b += "  %p = inttoptr i64 %a to ptr\n  %q = inttoptr i64 %b to ptr\n"
                     "  %c = call i32 @beans_dec_cmp(ptr %p, ptr %q)\n"
                     "  %e = icmp eq i32 %c, 0\n  %z = zext i1 %e to i64\n"
                     "  ret i64 %z\n";
                break;
            case Ty::bytes_:
                b += "  %p = inttoptr i64 %a to ptr\n  %q = inttoptr i64 %b to ptr\n"
                     "  %c = call i64 @beans_bytes_eq(ptr %p, ptr %q)\n  ret i64 %c\n";
                break;
            case Ty::enum_: {
                if (is_niche_option(t)) {
                    std::string inner = request_eq(t->args[0]);
                    b += "  %an = icmp eq i64 %a, 0\n"
                         "  %bn = icmp eq i64 %b, 0\n"
                         "  br i1 %an, label %anull, label %aval\n"
                         "anull:\n  %nz = zext i1 %bn to i64\n  ret i64 %nz\n"
                         "aval:\n  br i1 %bn, label %no, label %values\n"
                         "values:\n  %veq = call i64 " + inner +
                         "(i64 %a, i64 %b)\n  ret i64 %veq\n"
                         "no:\n  ret i64 0\n";
                    break;
                }
                std::vector<std::vector<Ty*>> pays; // per variant tag
                if (t->name == "Option") {
                    pays.push_back(t->args.empty() ? std::vector<Ty*>{}
                                                   : std::vector<Ty*>{t->args[0]});
                    pays.push_back({});
                } else if (const EnumDecl* d = enum_decls.count(t->name)
                                                   ? enum_decls[t->name]
                                                   : nullptr) {
                    std::map<std::string, Ty*> env;
                    for (size_t i = 0; i < d->generics.size() && i < t->args.size(); i++) {
                        env[d->generics[i].name] = t->args[i];
                    }
                    for (const EnumVariant& v : d->variants) {
                        std::vector<Ty*> ps;
                        for (const Param& p : v.payload) {
                            ps.push_back(resolve(p.type.get(), env, 0, 0));
                        }
                        pays.push_back(std::move(ps));
                    }
                }
                b += "  %ea = inttoptr i64 %a to ptr\n"
                     "  %eb = inttoptr i64 %b to ptr\n"
                     "  %ta = load i64, ptr %ea\n  %tb = load i64, ptr %eb\n"
                     "  %tc = icmp eq i64 %ta, %tb\n"
                     "  br i1 %tc, label %sw, label %no\nsw:\n";
                b += "  switch i64 %ta, label %yes [";
                for (size_t i = 0; i < pays.size(); i++) {
                    if (!pays[i].empty()) {
                        b += " i64 " + std::to_string(i) + ", label %v" +
                             std::to_string(i);
                    }
                }
                b += " ]\n";
                int r = 0;
                for (size_t i = 0; i < pays.size(); i++) {
                    if (pays[i].empty()) continue;
                    b += "v" + std::to_string(i) + ":\n";
                    for (size_t pi = 0; pi < pays[i].size(); pi++) {
                        std::string ps = request_eq(pays[i][pi]);
                        std::string pa = "%r" + std::to_string(r++);
                        std::string la = "%r" + std::to_string(r++);
                        std::string pb = "%r" + std::to_string(r++);
                        std::string lb = "%r" + std::to_string(r++);
                        std::string c = "%r" + std::to_string(r++);
                        std::string cc = "%r" + std::to_string(r++);
                        std::string off = std::to_string(8 + 8 * pi);
                        b += "  " + pa + " = getelementptr i8, ptr %ea, i64 " + off + "\n";
                        b += "  " + la + " = load i64, ptr " + pa + "\n";
                        b += "  " + pb + " = getelementptr i8, ptr %eb, i64 " + off + "\n";
                        b += "  " + lb + " = load i64, ptr " + pb + "\n";
                        b += "  " + c + " = call i64 " + ps + "(i64 " + la + ", i64 " +
                             lb + ")\n";
                        b += "  " + cc + " = icmp ne i64 " + c + ", 0\n";
                        bool last = pi + 1 == pays[i].size();
                        std::string next = last ? "yes"
                                                : "v" + std::to_string(i) + "_" +
                                                      std::to_string(pi + 1);
                        b += "  br i1 " + cc + ", label %" + next + ", label %no\n";
                        if (!last) b += next + ":\n";
                    }
                }
                b += "no:\n  ret i64 0\nyes:\n  ret i64 1\n";
                break;
            }
            default: // maps, files, threads, closures: value_eq's false arm
                b += "  ret i64 0\n";
                break;
        }
        b += "}\n";
        lifted += b;
        return sym;
    }

    // structural hash mirroring request_eq arm for arm: whatever @beqN calls
    // equal must hash equal, or the map index misses keys the linear scan
    // found. Memoized before the body so recursive enums close over themselves.
    std::map<Ty*, std::string> hash_fns;
    std::string request_hash(Ty* t) {
        auto it = hash_fns.find(t);
        if (it != hash_fns.end()) return it->second;
        std::string sym = "@bhash" + std::to_string(hash_fns.size());
        hash_fns[t] = sym;
        std::string b = "define internal i64 " + sym + "(i64 %a) {\n";
        switch (t->k) {
            case Ty::i64_:
            case Ty::i1_:
            case Ty::unit_:
            case Ty::list_:
            case Ty::obj_: // identity kinds hash the slot bits
                b += "  %h = call i64 @beans_slot_mix(i64 %a)\n  ret i64 %h\n";
                break;
            case Ty::f64_:
                b += "  %h = call i64 @beans_" +
                     std::string(t->bits == 32 ? "f32" : "f64") +
                     "_hash(i64 %a)\n  ret i64 %h\n";
                break;
            case Ty::str_:
                b += "  %p = inttoptr i64 %a to ptr\n"
                     "  %h = call i64 @beans_str_hash(ptr %p)\n  ret i64 %h\n";
                break;
            case Ty::dec_:
                b += "  %p = inttoptr i64 %a to ptr\n"
                     "  %h = call i64 @beans_dec_hash(ptr %p)\n  ret i64 %h\n";
                break;
            case Ty::bytes_:
                b += "  %p = inttoptr i64 %a to ptr\n"
                     "  %h = call i64 @beans_bytes_hash(ptr %p)\n  ret i64 %h\n";
                break;
            case Ty::enum_: {
                if (is_niche_option(t)) {
                    std::string inner = request_hash(t->args[0]);
                    b += "  %n = icmp eq i64 %a, 0\n"
                         "  br i1 %n, label %none, label %some\n"
                         "none:\n  %nh = call i64 @beans_slot_mix(i64 1)\n"
                         "  ret i64 %nh\n"
                         "some:\n  %sh = call i64 " + inner +
                         "(i64 %a)\n  ret i64 %sh\n";
                    break;
                }
                std::vector<std::vector<Ty*>> pays; // per variant tag
                if (t->name == "Option") {
                    pays.push_back(t->args.empty() ? std::vector<Ty*>{}
                                                   : std::vector<Ty*>{t->args[0]});
                    pays.push_back({});
                } else if (const EnumDecl* d = enum_decls.count(t->name)
                                                   ? enum_decls[t->name]
                                                   : nullptr) {
                    std::map<std::string, Ty*> env;
                    for (size_t i = 0; i < d->generics.size() && i < t->args.size(); i++) {
                        env[d->generics[i].name] = t->args[i];
                    }
                    for (const EnumVariant& v : d->variants) {
                        std::vector<Ty*> ps;
                        for (const Param& p : v.payload) {
                            ps.push_back(resolve(p.type.get(), env, 0, 0));
                        }
                        pays.push_back(std::move(ps));
                    }
                }
                b += "  %ea = inttoptr i64 %a to ptr\n"
                     "  %t = load i64, ptr %ea\n"
                     "  %h0 = call i64 @beans_slot_mix(i64 %t)\n";
                b += "  switch i64 %t, label %done [";
                for (size_t i = 0; i < pays.size(); i++) {
                    if (!pays[i].empty()) {
                        b += " i64 " + std::to_string(i) + ", label %v" +
                             std::to_string(i);
                    }
                }
                b += " ]\n";
                int r = 0;
                for (size_t i = 0; i < pays.size(); i++) {
                    if (pays[i].empty()) continue;
                    b += "v" + std::to_string(i) + ":\n";
                    std::string acc = "%h0";
                    for (size_t pi = 0; pi < pays[i].size(); pi++) {
                        std::string ps = request_hash(pays[i][pi]);
                        std::string pa = "%r" + std::to_string(r++);
                        std::string la = "%r" + std::to_string(r++);
                        std::string fh = "%r" + std::to_string(r++);
                        std::string mu = "%r" + std::to_string(r++);
                        std::string nx = "%r" + std::to_string(r++);
                        std::string off = std::to_string(8 + 8 * pi);
                        b += "  " + pa + " = getelementptr i8, ptr %ea, i64 " + off + "\n";
                        b += "  " + la + " = load i64, ptr " + pa + "\n";
                        b += "  " + fh + " = call i64 " + ps + "(i64 " + la + ")\n";
                        b += "  " + mu + " = mul i64 " + acc + ", 1099511628211\n";
                        b += "  " + nx + " = xor i64 " + mu + ", " + fh + "\n";
                        acc = nx;
                    }
                    b += "  ret i64 " + acc + "\n";
                }
                b += "done:\n  ret i64 %h0\n";
                break;
            }
            default: // never-equal kinds: any hash is consistent with eq
                b += "  ret i64 0\n";
                break;
        }
        b += "}\n";
        lifted += b;
        return sym;
    }
    struct FnInst {
        const FnDecl* decl;
        std::map<std::string, Ty*> env;
        std::string symbol;
    };
    std::vector<FnInst> fn_queue;
    std::set<std::string> fn_emitted;

    std::string request_fn(const FnDecl* decl, std::map<std::string, Ty*> env) {
        std::string sym = "@b_" + decl->qualname;
        for (const GenericParam& g : decl->generics) {
            auto it = env.find(g.name);
            sym += "$" + (it != env.end() ? mangle(it->second) : "x");
        }
        for (const FnInst& fi : fn_queue) {
            if (fi.symbol == sym) return sym;
        }
        fn_queue.push_back({decl, std::move(env), sym});
        return sym;
    }

    // structural match of a declared param type against a concrete arg type,
    // binding the fn's generic parameter names
    void unify_tref(const TypeRef* p, Ty* arg, const std::set<std::string>& gens,
                    std::map<std::string, Ty*>& env) {
        if (!p || !arg) return;
        if (p->kind == TypeRef::Kind::fn) {
            if (arg->k != Ty::fn_) return;
            for (size_t i = 0; i < p->fn_params.size() && i < arg->fn_nparams(); i++) {
                unify_tref(p->fn_params[i].get(), arg->args[i], gens, env);
            }
            if (p->fn_ret) unify_tref(p->fn_ret.get(), arg->fn_ret(), gens, env);
            return;
        }
        if (gens.count(p->name)) {
            if (!env.count(p->name)) env[p->name] = arg;
            return;
        }
        if (p->name == "List" && arg->k == Ty::list_ && p->args.size() == 1) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            return;
        }
        if ((p->name == "Map" || p->name == "OrderedMap") &&
            arg->k == Ty::map_ && p->args.size() == 2) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            unify_tref(p->args[1].get(), arg->args[1], gens, env);
            return;
        }
        if (p->name == "Box" && arg->k == Ty::box_ && p->args.size() == 1) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            return;
        }
        if (p->name == "Arena" && arg->k == Ty::arena_ && p->args.size() == 1) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            return;
        }
        if (p->name == "Shared" && arg->k == Ty::shared_ && p->args.size() == 1) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            return;
        }
        if (p->name == "Weak" && arg->k == Ty::weak_ && p->args.size() == 1) {
            unify_tref(p->args[0].get(), arg->args[0], gens, env);
            return;
        }
        if ((p->name == "Option" || p->name == "Result") && arg->k == Ty::enum_) {
            for (size_t i = 0; i < p->args.size() && i < arg->args.size(); i++) {
                unify_tref(p->args[i].get(), arg->args[i], gens, env);
            }
        }
    }
};

// ---- free-variable analysis (for closure capture) ----------------------------

struct FreeVars {
    std::set<std::string> bound;
    std::set<std::string> free;
    std::vector<std::unique_ptr<std::string>> srcs;

    void use(std::string_view name) {
        std::string n(name);
        if (!bound.count(n)) free.insert(n);
    }
    void block(const std::vector<StmtPtr>& b) {
        std::set<std::string> save = bound;
        for (const StmtPtr& s : b) stmt(s.get());
        bound = save;
    }
    void pat(const Pattern* p) {
        if (!p) return;
        if (p->kind == Pattern::Kind::name) {
            for (const Param& b : p->bindings) bound.insert(b.name);
        }
        for (const PatPtr& a : p->alts) pat(a.get());
    }
    void stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case Stmt::Kind::let_:
                expr(s->init.get());
                bound.insert(s->name);
                break;
            case Stmt::Kind::assign:
                expr(s->target.get());
                expr(s->value.get());
                break;
            case Stmt::Kind::expr:
            case Stmt::Kind::ret:
            case Stmt::Kind::defer_:
                expr(s->expr.get());
                break;
            case Stmt::Kind::if_:
                expr(s->cond.get());
                block(s->body);
                block(s->else_body);
                break;
            case Stmt::Kind::for_ever:
                block(s->body);
                break;
            case Stmt::Kind::for_while: {
                expr(s->cond.get());
                block(s->body);
                break;
            }
            case Stmt::Kind::for_in: {
                expr(s->iterable.get());
                std::set<std::string> save = bound;
                bound.insert(s->loop_var);
                block(s->body);
                bound = save;
                break;
            }
            case Stmt::Kind::unsafe_:
                block(s->body);
                break;
            default:
                break;
        }
    }
    void expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case Expr::Kind::ident:
                use(e->text);
                break;
            case Expr::Kind::string_lit: {
                // interpolation pieces reference variables too
                for (StrPiece& p : split_interp(e->text, srcs)) {
                    if (p.expr) expr(p.expr.get());
                }
                break;
            }
            case Expr::Kind::unary:
                expr(e->rhs.get());
                break;
            case Expr::Kind::binary:
            case Expr::Kind::range:
                expr(e->lhs.get());
                expr(e->rhs.get());
                break;
            case Expr::Kind::call:
                expr(e->callee.get());
                for (const ExprPtr& a : e->args) expr(a.get());
                break;
            case Expr::Kind::field:
                expr(e->object.get());
                break;
            case Expr::Kind::index:
                expr(e->object.get());
                expr(e->index_expr.get());
                break;
            case Expr::Kind::list_lit:
                for (const ExprPtr& a : e->args) expr(a.get());
                break;
            case Expr::Kind::init:
                for (const InitEntry& en : e->entries) {
                    expr(en.key.get());
                    expr(en.value.get());
                }
                break;
            case Expr::Kind::cast:
            case Expr::Kind::try_:
                expr(e->object.get());
                break;
            case Expr::Kind::closure: {
                std::set<std::string> save = bound;
                for (const Param& p : e->params) bound.insert(p.name);
                block(e->body);
                bound = save;
                break;
            }
            case Expr::Kind::if_expr:
                expr(e->cond.get());
                expr(e->then_e.get());
                expr(e->else_e.get());
                break;
            case Expr::Kind::match_expr: {
                expr(e->subject.get());
                for (const MatchArm& a : e->arms) {
                    std::set<std::string> save = bound;
                    pat(a.pat.get());
                    expr(a.value.get());
                    block(a.body);
                    bound = save;
                }
                break;
            }
            default:
                break;
        }
    }
};

// free names of a closure node (its own params/locals excluded)
static std::set<std::string> closure_free_names(const Expr* clo) {
    FreeVars fv;
    for (const Param& p : clo->params) fv.bound.insert(p.name);
    fv.block(clo->body);
    return fv.free;
}

// every name captured by any closure anywhere in this body
struct ClosureScan {
    std::set<std::string> captured;
    std::set<std::string> taken;
    void block(const std::vector<StmtPtr>& b) {
        for (const StmtPtr& s : b) stmt(s.get());
    }
    void stmt(const Stmt* s) {
        if (!s) return;
        expr(s->init.get());
        expr(s->target.get());
        expr(s->value.get());
        expr(s->expr.get());
        expr(s->cond.get());
        expr(s->iterable.get());
        block(s->body);
        block(s->else_body);
    }
    void expr(const Expr* e) {
        if (!e) return;
        if (e->kind == Expr::Kind::unary && e->op == TokenKind::kw_take && e->rhs &&
            e->rhs->kind == Expr::Kind::ident) {
            taken.insert(std::string(e->rhs->text));
        }
        if (e->kind == Expr::Kind::closure) {
            std::set<std::string> f = closure_free_names(e);
            captured.insert(f.begin(), f.end());
            // and keep scanning inside for doubly nested closures
        }
        if (e->kind == Expr::Kind::string_lit) {
            std::vector<std::unique_ptr<std::string>> srcs;
            for (StrPiece& p : split_interp(e->text, srcs)) {
                if (p.expr) expr(p.expr.get());
            }
            return;
        }
        expr(e->lhs.get());
        expr(e->rhs.get());
        expr(e->callee.get());
        for (const ExprPtr& a : e->args) expr(a.get());
        expr(e->object.get());
        expr(e->index_expr.get());
        for (const InitEntry& en : e->entries) {
            expr(en.key.get());
            expr(en.value.get());
        }
        expr(e->cond.get());
        expr(e->then_e.get());
        expr(e->else_e.get());
        expr(e->subject.get());
        for (const MatchArm& a : e->arms) {
            expr(a.value.get());
            block(a.body);
        }
        for (const StmtPtr& s : e->body) stmt(s.get());
    }
};

// Conservative name-use counts for last-use ownership transfer. Shadowed
// names are disabled; captured locals are already boxed and never qualify.
struct ReadScan {
    std::map<std::string, size_t> reads;
    std::map<std::string, size_t> declarations;
    void pattern(const Pattern* p) {
        if (!p) return;
        for (const Param& binding : p->bindings) declarations[binding.name] += 1;
        for (const PatPtr& alt : p->alts) pattern(alt.get());
    }
    void block(const std::vector<StmtPtr>& body) {
        for (const StmtPtr& stmt : body) scan_stmt(stmt.get());
    }
    void scan_stmt(const Stmt* stmt) {
        if (!stmt) return;
        if (stmt->kind == Stmt::Kind::let_) declarations[stmt->name] += 1;
        if (stmt->kind == Stmt::Kind::for_in) declarations[stmt->loop_var] += 1;
        expr(stmt->init.get());
        if (stmt->target && stmt->target->kind != Expr::Kind::ident)
            expr(stmt->target.get());
        expr(stmt->value.get());
        expr(stmt->expr.get());
        expr(stmt->cond.get());
        expr(stmt->iterable.get());
        block(stmt->body);
        block(stmt->else_body);
    }
    void expr(const Expr* e) {
        if (!e) return;
        if (e->kind == Expr::Kind::ident) {
            reads[std::string(e->text)] += 1;
            return;
        }
        if (e->kind == Expr::Kind::string_lit) {
            std::vector<std::unique_ptr<std::string>> srcs;
            for (StrPiece& piece : split_interp(e->text, srcs)) {
                if (piece.expr) expr(piece.expr.get());
            }
            return;
        }
        if (e->kind == Expr::Kind::closure) {
            for (const Param& param : e->params) declarations[param.name] += 1;
        }
        expr(e->lhs.get());
        expr(e->rhs.get());
        expr(e->callee.get());
        for (const ExprPtr& arg : e->args) expr(arg.get());
        expr(e->object.get());
        expr(e->index_expr.get());
        for (const InitEntry& entry : e->entries) {
            expr(entry.key.get());
            expr(entry.value.get());
        }
        expr(e->cond.get());
        expr(e->then_e.get());
        expr(e->else_e.get());
        expr(e->subject.get());
        for (const MatchArm& arm : e->arms) {
            pattern(arm.pat.get());
            expr(arm.value.get());
            block(arm.body);
        }
        block(e->body);
    }
};

// ---- per-function emitter ----------------------------------------------------

struct FnEmit {
    CG2& cg;
    std::string symbol;
    bool is_main;
    bool has_self;
    CImpl* self_impl;               // methods of a class
    const ClassDecl* self_iface;    // default methods of an interface
    const EnumDecl* self_enum;      // methods of an enum
    const std::map<std::string, Ty*>& env;

    const std::vector<Param>& params_ref;
    const TypeRef* ret_ref;
    const std::vector<StmtPtr>& body_ref;
    uint32_t dline, dcol;
    std::string fn_name; // declared method/fn name; "" for closures
    // a deinit whose class chain has an ancestor deinit calls it on every
    // return path, after cleanup, right before ret — subclass first, then up
    std::string deinit_chain;
    // closures: captured variables arrive as heap cells behind an env ptr
    const std::vector<std::pair<std::string, Ty*>>* captured = nullptr;

    std::string allocas, body, entry_inits;
    std::string pkg; // package prefix this function's source lives in
    int next_reg = 0, next_bb = 0;
    bool terminated = false;
    struct Var {
        std::string slot;
        std::string live_flag; // take-able unboxed ref: does this frame own it?
        Ty* ty;
        bool boxed = false;
        bool owned = false; // frame holds a ref (lets); params/bindings borrow
        bool inout = false; // slot aliases caller storage; never frame-owned
        std::string direct_fn; // known closure target for never-reassigned locals
        int seq = 0;        // declaration order — deinit made release order
                            // observable, and the interpreter dies newest-first
        size_t loop_depth = 0;
        int decimal_scale = -1; // exact only while every path preserves it
    };
    int next_seq = 0;
    std::vector<std::map<std::string, Var>> scopes;
    struct LoopCtx { std::string brk, cont; size_t scope_depth; };
    std::vector<LoopCtx> loop_stack;
    std::vector<std::unique_ptr<std::string>> interp_srcs;
    using EV = std::pair<std::string, Ty*>;
    // owned temporaries created while emitting the current statement
    std::vector<EV> temps;
    Ty* ret_ty = nullptr;
    std::set<std::string> boxed_names; // captured by closures
    std::set<std::string> taken_names;
    std::map<std::string, size_t> remaining_reads;
    std::map<std::string, size_t> declaration_counts;
    std::map<std::string, Var*> transfer_candidates;
    std::map<const Expr*, std::string> closure_symbols;
    struct DeferRec {
        const Expr* expr;
        std::string flag; // armed?
        std::vector<std::map<std::string, Var>> scope_snap; // names visible at the site
    };
    std::vector<DeferRec> defers;

    FnEmit(CG2& cg, const FnDecl& d, std::string sym, bool main, CImpl* si,
           const ClassDecl* ifc, const EnumDecl* en, const std::map<std::string, Ty*>& e)
        : cg(cg), symbol(std::move(sym)), is_main(main), has_self(d.has_self),
          self_impl(si), self_iface(ifc), self_enum(en), env(e), params_ref(d.params),
          ret_ref(d.ret.get()), body_ref(d.body), dline(d.line), dcol(d.col),
          fn_name(d.name) {}

    // closure form
    FnEmit(CG2& cg, const Expr* clo, std::string sym,
           const std::vector<std::pair<std::string, Ty*>>* caps,
           const std::map<std::string, Ty*>& e)
        : cg(cg), symbol(std::move(sym)), is_main(false), has_self(false),
          self_impl(nullptr), self_iface(nullptr), self_enum(nullptr), env(e),
          params_ref(clo->params), ret_ref(clo->type.get()), body_ref(clo->body),
          dline(clo->line), dcol(clo->col), captured(caps) {}

    std::string reg() { return "%t" + std::to_string(next_reg++); }
    std::string bb() { return "bb" + std::to_string(next_bb++); }
    void line(const std::string& s) { body += "  " + s + "\n"; }
    void label(const std::string& l) { body += l + ":\n"; terminated = false; }
    void err(const Expr* e, const std::string& what) {
        cg.err(e ? e->line : dline, e ? e->col : dcol, what);
    }

    Ty* rt(const TypeRef* t, uint32_t line, uint32_t col) {
        return cg.resolve(t, env, line, col);
    }

    Var* find_var(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // ---- borrow-alias elision ----
    // `var q: T = p` is a pure alias when neither name is ever reassigned in
    // this function: q borrows p's ref instead of taking its own pair. The
    // source outlives q by scoping (it is visible at q's declaration), so the
    // borrow can't dangle. Closure-captured (boxed) names keep the retain —
    // their cell owns its value and closures can swap it.
    std::set<std::string> assigned_names_;
    bool assigned_scanned_ = false;
    void scan_assigned(const std::vector<StmtPtr>& stmts) {
        for (const StmtPtr& sp : stmts) {
            const Stmt* s = sp.get();
            if (s->kind == Stmt::Kind::assign && s->target &&
                s->target->kind == Expr::Kind::ident) {
                assigned_names_.insert(std::string(s->target->text));
            }
            if (s->kind == Stmt::Kind::expr && s->expr &&
                s->expr->kind == Expr::Kind::match_expr) {
                for (const MatchArm& a : s->expr->arms) scan_assigned(a.body);
            }
            scan_assigned(s->body);
            scan_assigned(s->else_body);
        }
    }
    bool ever_assigned(const std::string& name) {
        if (!assigned_scanned_) {
            assigned_scanned_ = true;
            scan_assigned(body_ref);
        }
        return assigned_names_.count(name) > 0;
    }
    std::string alloc_slot(const std::string& name, Ty* t, bool entry = false) {
        bool boxed = boxed_names.count(name) > 0;
        std::string slot = "%v" + std::to_string(next_reg++) + "." + name;
        allocas += "  " + slot + " = alloca " + (boxed ? "ptr" : ll(t)) + "\n";
        std::string live_flag;
        if (!boxed && has_owned_refs(t)) {
            live_flag = "%v" + std::to_string(next_reg++) + "." + name + ".live";
            allocas += "  " + live_flag + " = alloca i1\n";
        }
        if (boxed) {
            // the variable lives in a heap cell so closures share it.
            // params get their cell in the entry block; lets get a fresh cell
            // at their statement site.
            std::string cell = "%cell" + std::to_string(next_reg++);
            if (has_owned_refs(t) && !pointer_mask_fits(t))
                cg.err(dline, dcol,
                       "capturing this inline value exceeds ARC metadata capacity");
            long long mask = pointer_mask(t);
            long long cmeta = mask ? fixed_meta(mask) : 0;
            std::string code = "  " + cell + " = call ptr @beans_alloc(i64 " +
                               std::to_string(CG2::value_size(t)) + ", i64 " +
                               std::to_string(cmeta) + ")\n" +
                               "  store ptr " + cell + ", ptr " + slot + "\n";
            if (entry) entry_inits += code;
            else body += code;
        }
        scopes.back()[name] = {slot, live_flag, t, boxed, false, false, "", next_seq++,
                               loop_stack.size()};
        return slot;
    }
    std::string fresh_slot(const char* tag, Ty* t) {
        std::string slot = "%v" + std::to_string(next_reg++) + "." + tag;
        allocas += "  " + slot + " = alloca " + ll(t) + "\n";
        return slot;
    }

    // ---- ownership bookkeeping ----
    void own(const std::string& val, Ty* t) {
        if (has_owned_refs(t) && !val.empty() && val[0] == '%')
            temps.push_back({val, t});
    }
    bool consume(const std::string& val) {
        for (auto it = temps.rbegin(); it != temps.rend(); ++it) {
            if (it->first == val) {
                temps.erase(std::next(it).base());
                return true;
            }
        }
        return false;
    }
    bool is_temp(const std::string& val) const {
        for (const EV& t : temps) {
            if (t.first == val) return true;
        }
        return false;
    }
    void emit_retain(const std::string& val) {
        line("call void @beans_retain(ptr " + val + ")");
    }
    void emit_release(const std::string& val) {
        line("call void @beans_release(ptr " + val + ")");
    }
    void emit_retain_value(const std::string& val, Ty* type) {
        if (is_rc(type)) {
            emit_retain(val);
            return;
        }
        if (is_inline_option(type)) {
            if (!has_owned_refs(type->args[0])) return;
            std::string payload = reg();
            line(payload + " = extractvalue " + std::string(ll(type)) + " " + val +
                 ", 1");
            emit_retain_value(payload, type->args[0]);
            return;
        }
        if (is_inline_result(type)) {
            for (size_t i = 0; i < 2; i++) {
                if (!has_owned_refs(type->args[i])) continue;
                std::string payload = reg();
                line(payload + " = extractvalue " + std::string(ll(type)) + " " +
                     val + ", " + std::to_string(i + 1));
                emit_retain_value(payload, type->args[i]);
            }
        }
    }
    void emit_release_value(const std::string& val, Ty* type) {
        if (is_rc(type)) {
            emit_release(val);
            return;
        }
        if (is_inline_option(type)) {
            if (!has_owned_refs(type->args[0])) return;
            std::string payload = reg();
            line(payload + " = extractvalue " + std::string(ll(type)) + " " + val +
                 ", 1");
            emit_release_value(payload, type->args[0]);
            return;
        }
        if (is_inline_result(type)) {
            for (size_t i = 0; i < 2; i++) {
                if (!has_owned_refs(type->args[i])) continue;
                std::string payload = reg();
                line(payload + " = extractvalue " + std::string(ll(type)) + " " +
                     val + ", " + std::to_string(i + 1));
                emit_release_value(payload, type->args[i]);
            }
        }
    }
    void emit_retain_value_entry(const std::string& val, Ty* type) {
        if (is_rc(type)) {
            entry_inits += "  call void @beans_retain(ptr " + val + ")\n";
            return;
        }
        if (is_inline_option(type)) {
            if (!has_owned_refs(type->args[0])) return;
            std::string payload = reg();
            entry_inits += "  " + payload + " = extractvalue " + ll(type) + " " + val +
                           ", 1\n";
            emit_retain_value_entry(payload, type->args[0]);
            return;
        }
        if (is_inline_result(type)) {
            for (size_t i = 0; i < 2; i++) {
                if (!has_owned_refs(type->args[i])) continue;
                std::string payload = reg();
                entry_inits += "  " + payload + " = extractvalue " + ll(type) + " " +
                               val + ", " + std::to_string(i + 1) + "\n";
                emit_retain_value_entry(payload, type->args[i]);
            }
        }
    }
    // release owned temps created since `mark`
    void flush_temps(size_t mark) {
        while (temps.size() > mark) {
            emit_release_value(temps.back().first, temps.back().second);
            temps.pop_back();
        }
    }
    // release the frame-owned locals of scopes [from_depth, end)
    void release_scopes(size_t from_depth, const std::string& except_slot = "") {
        for (size_t si = scopes.size(); si-- > from_depth;) {
            // newest declaration first — the scope map is alphabetical, but
            // deinit made release order observable and the interpreter's
            // frames die newest-first
            std::vector<const Var*> ordered;
            for (auto& [name, v] : scopes[si]) ordered.push_back(&v);
            std::sort(ordered.begin(), ordered.end(),
                      [](const Var* a, const Var* b) { return a->seq > b->seq; });
            for (const Var* v : ordered) {
                if (!except_slot.empty() && v->slot == except_slot) continue;
                if (v->boxed) {
                    if (v->owned) {
                        std::string cell = reg();
                        line(cell + " = load ptr, ptr " + v->slot);
                        emit_release(cell);
                    }
                } else if (v->owned && has_owned_refs(v->ty)) {
                    if (v->live_flag.empty()) {
                        std::string val = reg();
                        line(val + " = load " + std::string(ll(v->ty)) + ", ptr " +
                             v->slot);
                        emit_release_value(val, v->ty);
                    } else {
                        std::string live = reg();
                        line(live + " = load i1, ptr " + v->live_flag);
                        std::string drop = bb(), done = bb();
                        line("br i1 " + live + ", label %" + drop + ", label %" + done);
                        label(drop);
                        std::string val = reg();
                        line(val + " = load " + std::string(ll(v->ty)) + ", ptr " +
                             v->slot);
                        emit_release_value(val, v->ty);
                        line("br label %" + done);
                        label(done);
                    }
                }
            }
        }
    }

    // read/write a Var, transparently going through its cell when boxed
    std::string var_read(Var* v) {
        std::string r = reg();
        if (!v->boxed) {
            line(r + " = load " + std::string(ll(v->ty)) + ", ptr " + v->slot);
            return r;
        }
        std::string cell = reg();
        line(cell + " = load ptr, ptr " + v->slot);
        line(r + " = load " + std::string(ll(v->ty)) + ", ptr " + cell);
        return r;
    }
    std::string var_ptr(Var* v) {
        if (!v->boxed) return v->slot;
        std::string cell = reg();
        line(cell + " = load ptr, ptr " + v->slot);
        return cell;
    }

    // Reads borrow — that is the discipline — but a borrow handed into a call
    // (argument, method receiver, match subject) can outlive its owner: the
    // callee may overwrite the container slot or object field it came from,
    // releasing the value while the caller still holds the raw pointer. Pin
    // exactly that shape: container/field reads get one retain and die with
    // the statement's temps. Idents, literals, and already-owned results are
    // skipped, so nothing lands on paths that did not need it.
    void pin_borrow(const Expr* a, const EV& v) {
        if (!a || !has_owned_refs(v.second)) return;
        if (a->kind != Expr::Kind::index && a->kind != Expr::Kind::field) return;
        if (is_temp(v.first)) return;
        emit_retain_value(v.first, v.second);
        own(v.first, v.second);
    }

    // equality kind for the C helpers (slot_eq/map_find), mirroring the
    // interpreter's value_eq: 0 raw slot (ints, bools, unit, pointer-identity
    // lists/objects), 1 f64 by IEEE value, 2 string content, 3 decimal value,
    // 4 generated structural thunk (enums, Bytes), 5 never equal (maps and
    // resource handles), 6 f32 by IEEE value
    int eq_kind(Ty* t) {
        switch (t->k) {
            case Ty::f64_: return t->bits == 32 ? 6 : 1;
            case Ty::str_: return 2;
            case Ty::dec_: return 3;
            case Ty::enum_:
            case Ty::bytes_: return 4;
            case Ty::i64_:
            case Ty::i1_:
            case Ty::unit_:
            case Ty::list_:
            case Ty::box_:
            case Ty::arena_:
            case Ty::shared_:
            case Ty::weak_:
            case Ty::rawptr_:
            case Ty::obj_: return 0;
            default: return 5;
        }
    }
    std::string eq_thunk(Ty* t, int kind) { return kind == 4 ? cg.request_eq(t) : "null"; }
    // map index hash for thunk-compared keys; other kinds hash in the runtime
    std::string hash_thunk(Ty* t, int kind) { return kind == 4 ? cg.request_hash(t) : "null"; }
    // interp panics on integer divide/modulo by zero; a bare sdiv would give 0
    // silently on arm64 (and trap on x86) — emit the same panic on both paths
    void guard_div_zero(const std::string& rhs, Ty* type, bool is_mod,
                        uint32_t ln, uint32_t cl) {
        std::string c = reg();
        line(c + " = icmp eq " + std::string(ll(type)) + " " + rhs + ", 0");
        std::string bad = bb(), ok = bb();
        line("br i1 " + c + ", label %" + bad + ", label %" + ok);
        label(bad);
        line("call void @beans_panic(ptr " +
             cg.intern_string(is_mod ? "modulo by zero" : "divide by zero") +
             ", i64 " + std::to_string(ln) + ", i64 " + std::to_string(cl) + ")");
        line("unreachable");
        label(ok);
    }
    // ordering kind for sort/min/max — non-ordered element types (reachable
    // through generic instantiation) get kind 4: slot_cmp answers "equal", so
    // sort keeps the original order and min/max return the first element,
    // exactly like the interpreter's value_less returning false
    int order_kind(Ty* t) {
        switch (t->k) {
            case Ty::f64_: return t->bits == 32 ? 6 : 1;
            case Ty::str_: return 2;
            case Ty::dec_: return 3;
            case Ty::i64_:
                return t->is_unsigned ? 5 : 0;
            case Ty::i1_: return 0;
            default: return 4;
        }
    }

    // value is about to be stored somewhere that owns it: transfer or +1
    void transfer_in(const EV& v) {
        if (!has_owned_refs(v.second)) return;
        if (consume(v.first)) return; // ownership moves
        auto candidate = transfer_candidates.find(v.first);
        if (candidate != transfer_candidates.end()) {
            Var* source = candidate->second;
            if (!source->live_flag.empty()) line("store i1 0, ptr " + source->live_flag);
            transfer_candidates.erase(candidate);
            return;
        }
        emit_retain_value(v.first, v.second);
    }

    // ---- allocation helpers ----
    std::string alloc_bytes(int n, long long meta) {
        std::string r = reg();
        line(r + " = call ptr @beans_alloc(i64 " + std::to_string(n) + ", i64 " +
             std::to_string(meta) + ")");
        return r;
    }
    // mask bits land in meta bits 3..60; 61-63 belong to the cycle collector
    static long long fixed_meta(long long mask) { return 1 | (mask << 3); }
    static long long pointer_mask(Ty* type, int base = 0) {
        if (!type) return 0;
        if (is_rc(type)) {
            int slot = base / 8;
            return base % 8 == 0 && slot < 58 ? static_cast<long long>(1ULL << slot)
                                               : 0;
        }
        if (is_inline_option(type)) {
            int offset = CG2::align_up(1, CG2::value_align(type->args[0]));
            return pointer_mask(type->args[0], base + offset);
        }
        if (is_inline_result(type)) {
            int ok_offset = CG2::align_up(1, CG2::value_align(type->args[0]));
            int error_offset = CG2::align_up(
                ok_offset + CG2::value_size(type->args[0]),
                CG2::value_align(type->args[1]));
            return pointer_mask(type->args[0], base + ok_offset) |
                   pointer_mask(type->args[1], base + error_offset);
        }
        return 0;
    }
    static bool pointer_mask_fits(Ty* type, int base = 0) {
        if (!type) return true;
        if (is_rc(type)) return base % 8 == 0 && base / 8 < 58;
        if (is_inline_option(type)) {
            int offset = CG2::align_up(1, CG2::value_align(type->args[0]));
            return pointer_mask_fits(type->args[0], base + offset);
        }
        if (is_inline_result(type)) {
            int ok_offset = CG2::align_up(1, CG2::value_align(type->args[0]));
            int error_offset = CG2::align_up(
                ok_offset + CG2::value_size(type->args[0]),
                CG2::value_align(type->args[1]));
            return pointer_mask_fits(type->args[0], base + ok_offset) &&
                   pointer_mask_fits(type->args[1], base + error_offset);
        }
        return true;
    }
    void store_at(const std::string& base, int offset, const std::string& val, Ty* t) {
        std::string p = reg();
        line(p + " = getelementptr i8, ptr " + base + ", i64 " + std::to_string(offset));
        line("store " + std::string(ll(t)) + " " + val + ", ptr " + p);
    }
    std::string load_at(const std::string& base, int offset, Ty* t) {
        std::string p = reg(), r = reg();
        line(p + " = getelementptr i8, ptr " + base + ", i64 " + std::to_string(offset));
        line(r + " = load " + std::string(ll(t)) + ", ptr " + p);
        return r;
    }
    std::string load_slot_at(const std::string& base, int offset, Ty* type) {
        return from_slot(load_at(base, offset, cg.t_i64()), type);
    }
    // enum boxes own their payload refs; the box itself is an owned temp
    std::string box_enum(int tag, const std::vector<EV>& payload) {
        if (payload.empty()) return cg.intern_enum_tag(tag); // immortal singleton
        long long mask = 0;
        for (size_t i = 0; i < payload.size(); i++) {
            if (is_slot_rc(payload[i].second)) mask |= 1LL << (i + 1);
        }
        std::string b = alloc_bytes(8 + 8 * static_cast<int>(payload.size()),
                                    fixed_meta(mask));
        store_at(b, 0, std::to_string(tag), cg.t_i64());
        for (size_t i = 0; i < payload.size(); i++) {
            transfer_in(payload[i]);
            store_at(b, 8 + 8 * static_cast<int>(i), to_slot(payload[i], true),
                     cg.t_i64());
        }
        own(b, cg.t_enum("Option", {})); // any rc type works for bookkeeping
        return b;
    }
    std::string make_option_some(const EV& value, Ty* inner) {
        Ty* opt = cg.t_option(inner);
        if (is_inline_option(opt)) {
            transfer_in(value);
            std::string tagged = reg(), result = reg();
            line(tagged + " = insertvalue " + std::string(ll(opt)) +
                 " zeroinitializer, i1 true, 0");
            line(result + " = insertvalue " + std::string(ll(opt)) + " " + tagged +
                 ", " + ll(inner) + " " + value.first + ", 1");
            own(result, opt);
            return result;
        }
        if (!is_niche_option(opt)) return box_enum(0, {value});
        transfer_in(value);
        own(value.first, opt);
        return value.first;
    }
    std::string make_option_none(Ty* inner) {
        Ty* opt = cg.t_option(inner);
        if (is_inline_option(opt)) return "zeroinitializer";
        return is_niche_option(opt) ? "null" : box_enum(1, {});
    }

    std::string make_result_value(bool ok, const EV& value, Ty* ok_type,
                                  Ty* error_type) {
        Ty* result_type = cg.t_result(ok_type, error_type);
        if (!is_inline_result(result_type))
            return box_enum(ok ? 0 : 1, {value});
        transfer_in(value);
        std::string tagged = reg(), result = reg();
        line(tagged + " = insertvalue " + std::string(ll(result_type)) +
             " zeroinitializer, i1 " + (ok ? "false" : "true") + ", 0");
        size_t field = ok ? 1 : 2;
        line(result + " = insertvalue " + std::string(ll(result_type)) + " " + tagged +
             ", " + ll(value.second) + " " + value.first + ", " +
             std::to_string(field));
        own(result, result_type);
        return result;
    }

    std::string result_is_ok(const EV& result) {
        std::string is_ok = reg();
        if (is_inline_result(result.second)) {
            std::string is_error = reg();
            line(is_error + " = extractvalue " + std::string(ll(result.second)) + " " +
                 result.first + ", 0");
            line(is_ok + " = xor i1 " + is_error + ", true");
        } else {
            std::string tag = load_at(result.first, 0, cg.t_i64());
            line(is_ok + " = icmp eq i64 " + tag + ", 0");
        }
        return is_ok;
    }

    std::string result_payload(const EV& result, bool ok) {
        Ty* payload_type = result.second->args[ok ? 0 : 1];
        if (is_inline_result(result.second)) {
            std::string payload = reg();
            line(payload + " = extractvalue " + std::string(ll(result.second)) + " " +
                 result.first + ", " + std::to_string(ok ? 1 : 2));
            return payload;
        }
        return load_slot_at(result.first, 8, payload_type);
    }

    std::string option_has(const EV& option) {
        std::string result = reg();
        if (is_inline_option(option.second)) {
            line(result + " = extractvalue " + std::string(ll(option.second)) + " " +
                 option.first + ", 0");
        } else if (is_niche_option(option.second)) {
            line(result + " = icmp ne ptr " + option.first + ", null");
        } else {
            std::string tag = load_at(option.first, 0, cg.t_i64());
            line(result + " = icmp eq i64 " + tag + ", 0");
        }
        return result;
    }

    std::string option_payload(const EV& option, Ty* inner) {
        if (is_inline_option(option.second)) {
            std::string result = reg();
            line(result + " = extractvalue " + std::string(ll(option.second)) + " " +
                 option.first + ", 1");
            return result;
        }
        if (is_niche_option(option.second)) return option.first;
        return load_slot_at(option.first, 8, inner);
    }

    // Convert to the generic runtime's i64 slot. Decimal is inline everywhere
    // else, but collections still have an eight-byte ABI, so make a small RC
    // box here. A storing call takes that box; a lookup keeps it as a temporary.
    std::string to_slot(const EV& v, bool storing = false) {
        if (v.second->k == Ty::i64_) {
            if (!v.second->bits || v.second->bits == 64) return v.first;
            std::string r = reg();
            line(r + " = " + (v.second->is_unsigned ? "zext" : "sext") + " " +
                 std::string(ll(v.second)) + " " + v.first + " to i64");
            return r;
        }
        std::string r = reg();
        if (v.second->k == Ty::dec_) {
            std::string box = reg();
            line(box + " = call ptr @beans_decv_box(i128 " + v.first + ")");
            if (!storing) own(box, cg.t_str());
            line(r + " = ptrtoint ptr " + box + " to i64");
            return r;
        }
        if (v.second->k == Ty::f64_) {
            if (v.second->bits == 32) {
                std::string raw = reg();
                line(raw + " = bitcast float " + v.first + " to i32");
                line(r + " = zext i32 " + raw + " to i64");
            } else {
                line(r + " = bitcast double " + v.first + " to i64");
            }
        } else if (v.second->k == Ty::i1_) {
            line(r + " = zext i1 " + v.first + " to i64");
        } else {
            line(r + " = ptrtoint ptr " + v.first + " to i64");
        }
        return r;
    }
    std::string from_slot(const std::string& v, Ty* t, bool taking = false) {
        if (t->k == Ty::i64_) {
            if (!t->bits || t->bits == 64) return v;
            std::string r = reg();
            line(r + " = trunc i64 " + v + " to " + std::string(ll(t)));
            return r;
        }
        std::string r = reg();
        if (t->k == Ty::dec_) {
            std::string box = reg();
            line(box + " = inttoptr i64 " + v + " to ptr");
            line(r + " = call i128 @beans_decv_unbox(ptr " + box + ")");
            if (taking) emit_release(box);
            return r;
        }
        if (t->k == Ty::f64_) {
            if (t->bits == 32) {
                std::string raw = reg();
                line(raw + " = trunc i64 " + v + " to i32");
                line(r + " = bitcast i32 " + raw + " to float");
            } else {
                line(r + " = bitcast i64 " + v + " to double");
            }
        }
        else if (t->k == Ty::i1_) line(r + " = trunc i64 " + v + " to i1");
        else line(r + " = inttoptr i64 " + v + " to ptr");
        return r;
    }
    std::string as_f64(const EV& value) {
        if (value.second->k != Ty::f64_ || value.second->bits != 32) return value.first;
        std::string wide = reg();
        line(wide + " = fpext float " + value.first + " to double");
        return wide;
    }
    std::string normalize_integer(const std::string& value, Ty* type) {
        (void)type;
        return value;
    }
    std::string normalize_float(const std::string& value, Ty* type) {
        (void)type;
        return value;
    }

    // ---- expressions -------------------------------------------------------
    // (compiles the expression to IR; executes nothing)
    EV eval(const Expr* e, Ty* hint = nullptr) {
        switch (e->kind) {
            case Expr::Kind::int_lit: {
                TypeId checked = cg.checked_type(e);
                if (checked && checked->k == Type::K::decimal_)
                    return dec_literal(e->text);
                if (Ty* typed = cg.checked_primitive(e)) {
                    if (typed->k == Ty::f64_) {
                        double value = parse_float_text(e->text);
                        if (typed->bits == 32) value = static_cast<float>(value);
                        return {fmt_double(value), typed};
                    }
                    if (typed->k == Ty::i64_)
                        return {std::to_string(parse_int_text(e->text)), typed};
                }
                if (hint && hint->k == Ty::dec_) return dec_literal(e->text);
                if (hint && hint->k == Ty::f64_) {
                    double value = parse_float_text(e->text);
                    if (hint->bits == 32) value = static_cast<float>(value);
                    return {fmt_double(value), hint};
                }
                return {std::to_string(parse_int_text(e->text)),
                        hint && hint->k == Ty::i64_ ? hint : cg.t_i64()};
            }
            case Expr::Kind::float_lit: {
                TypeId checked = cg.checked_type(e);
                if (checked && checked->k == Type::K::decimal_)
                    return dec_literal(e->text);
                if (Ty* typed = cg.checked_primitive(e)) {
                    if (typed->k == Ty::f64_) {
                        double value = parse_float_text(e->text);
                        if (typed->bits == 32) value = static_cast<float>(value);
                        return {fmt_double(value), typed};
                    }
                }
                if (hint && hint->k == Ty::dec_) return dec_literal(e->text);
                double value = parse_float_text(e->text);
                Ty* type = hint && hint->k == Ty::f64_ ? hint : cg.t_f64();
                if (type->bits == 32) value = static_cast<float>(value);
                return {fmt_double(value), type};
            }
            case Expr::Kind::bool_lit:
                return {e->bool_val ? "1" : "0", cg.t_bool()};
            case Expr::Kind::string_lit:
                return eval_string(e);
            case Expr::Kind::ident: {
                std::string name(e->text);
                if (Var* v = find_var(name)) {
                    std::string value = var_read(v);
                    auto remaining = remaining_reads.find(name);
                    if (remaining != remaining_reads.end() && remaining->second > 0)
                        remaining->second -= 1;
                    bool last_read = remaining != remaining_reads.end() &&
                                     remaining->second == 0 &&
                                     declaration_counts[name] == 1 &&
                                     v->loop_depth == loop_stack.size();
                    bool overwritten_next =
                        cg.hir.mir().can_move_before_overwrite(e);
                    if ((last_read || overwritten_next) && v->owned && !v->boxed &&
                        has_owned_refs(v->ty)) {
                        transfer_candidates[value] = v;
                    }
                    return {value, v->ty};
                }
                if (name == "none") {
                    Ty* inner = hint && hint->k == Ty::enum_ && !hint->args.empty()
                                    ? hint->args[0]
                                    : cg.t_i64();
                    std::string b = make_option_none(inner);
                    return {b, cg.t_option(inner)};
                }
                auto function = cg.fn_decls.find(
                    e->resolved.empty() ? cg.qual(name) : e->resolved);
                if (function != cg.fn_decls.end())
                    return eval_top_fn_ref(e, function->second);
                err(e, "reading '" + name + "'");
                return {"0", cg.t_i64()};
            }
            case Expr::Kind::self_ref: {
                if (Var* v = find_var("self")) {
                    return {var_read(v), v->ty};
                }
                err(e, "self");
                return {"null", cg.t_bad()};
            }
            case Expr::Kind::unary: {
                if (e->op == TokenKind::kw_inout) {
                    if (!e->rhs || e->rhs->kind != Expr::Kind::ident) {
                        err(e, "inout here");
                        return {"null", cg.t_bad()};
                    }
                    Var* var = find_var(std::string(e->rhs->text));
                    if (!var) {
                        err(e, "inout local");
                        return {"null", cg.t_bad()};
                    }
                    return {var_ptr(var), var->ty};
                }
                if (e->op == TokenKind::kw_take) {
                    if (!e->rhs || e->rhs->kind != Expr::Kind::ident) {
                        err(e, "take here");
                        return {"0", cg.t_i64()};
                    }
                    Var* var = find_var(std::string(e->rhs->text));
                    if (!var) {
                        err(e, "taking this local");
                        return {"0", cg.t_i64()};
                    }
                    std::string value = var_read(var);
                    if (has_owned_refs(var->ty)) {
                        if (var->boxed) {
                            line("store " + std::string(ll(var->ty)) +
                                 (is_rc(var->ty) ? " null, ptr "
                                                : " zeroinitializer, ptr ") +
                                 var_ptr(var));
                        } else if (!var->live_flag.empty()) {
                            line("store i1 0, ptr " + var->live_flag);
                        } else if (!var->owned) {
                            emit_retain_value(value, var->ty);
                        }
                        own(value, var->ty);
                    }
                    return {value, var->ty};
                }
                if (e->op == TokenKind::minus) {
                    EV v = eval(e->rhs.get(), hint);
                    std::string r = reg();
                    if (v.second->k == Ty::f64_) {
                        line(r + " = fneg " + std::string(ll(v.second)) + " " + v.first);
                        r = normalize_float(r, v.second);
                    } else if (v.second->k == Ty::dec_) {
                        line(r + " = call i128 @beans_decv_neg(i128 " + v.first + ")");
                    } else {
                        line(r + " = sub " + std::string(ll(v.second)) + " 0, " + v.first);
                        r = normalize_integer(r, v.second);
                    }
                    return {r, v.second};
                }
                if (e->op == TokenKind::bang) {
                    EV v = eval(e->rhs.get());
                    std::string r = reg();
                    line(r + " = xor i1 " + v.first + ", 1");
                    return {r, cg.t_bool()};
                }
                EV v = eval(e->rhs.get());
                std::string r = reg();
                line(r + " = xor " + std::string(ll(v.second)) + " " + v.first + ", -1");
                return {r, v.second};
            }
            case Expr::Kind::binary:
                return eval_binary(e);
            case Expr::Kind::range:
                err(e, "a range outside a for loop");
                return {"0", cg.t_i64()};
            case Expr::Kind::call:
                return eval_call(e, hint);
            case Expr::Kind::field:
                return eval_field(e);
            case Expr::Kind::index: {
                EV obj = eval(e->object.get());
                // the key must be built as the map's key type — a bare 1.5 on
                // a Map<decimal,_> read has to become a BDec, not float bits
                Ty* ih = obj.second->k == Ty::map_    ? obj.second->args[0]
                         : (obj.second->k == Ty::list_ ||
                            obj.second->k == Ty::fixed_array_ ||
                            obj.second->k == Ty::slice_) ? cg.t_i64()
                                                      : nullptr;
                EV idx = eval(e->index_expr.get(), ih);
                if (obj.second->k == Ty::map_) {
                    // map read: the value, or a panic naming the missing key —
                    // message byte-identical to the interpreter's
                    Ty* K = obj.second->args[0];
                    Ty* V = obj.second->args[1];
                    int kind = eq_kind(K);
                    std::string raw = reg();
                    std::string okv = reg(), c = reg();
                    if (kind == 0) {
                        std::string pair = reg();
                        line(pair + " = call {i64, i64} @beans_map_get_raw(ptr " +
                             obj.first + ", i64 " + to_slot(idx) + ")");
                        line(raw + " = extractvalue {i64, i64} " + pair + ", 0");
                        line(okv + " = extractvalue {i64, i64} " + pair + ", 1");
                    } else {
                        std::string okf = fresh_slot("mgok", cg.t_i64());
                        line(raw + " = call i64 @beans_map_get(ptr " + obj.first +
                             ", i64 " + to_slot(idx) + ", i64 " +
                             std::to_string(kind) + ", ptr " + okf + ", ptr " +
                             eq_thunk(K, kind) + ", ptr " + hash_thunk(K, kind) + ")");
                        line(okv + " = load i64, ptr " + okf);
                    }
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string okb = bb(), badb = bb();
                    line("br i1 " + c + ", label %" + okb + ", label %" + badb);
                    label(badb);
                    std::string ks = to_str(idx, e->index_expr.get());
                    // branch-local, and the panic never returns — but to_str is
                    // identity on strings, so this can eat the key temp itself,
                    // and then the hit path must release it in its own branch
                    bool key_eaten = consume(ks) && ks == idx.first;
                    std::string m = reg();
                    line(m + " = call ptr @beans_concat(ptr " +
                         cg.intern_string("map key not found: ") + ", ptr " + ks + ")");
                    line("call void @beans_panic(ptr " + m + ", i64 " +
                         std::to_string(e->line) + ", i64 " + std::to_string(e->col) +
                         ")");
                    line("unreachable");
                    label(okb);
                    if (key_eaten) emit_release(idx.first);
                    return {from_slot(raw, V), V}; // borrowed from the map, like lists
                }
                if (obj.second->k == Ty::slice_) {
                    Ty* element = obj.second->args[0];
                    std::string pointer = reg(), length = reg();
                    line(pointer + " = extractvalue {ptr, i64} " + obj.first + ", 0");
                    line(length + " = extractvalue {ptr, i64} " + obj.first + ", 1");
                    std::string index = to_slot(idx), inside = reg();
                    line(inside + " = icmp ult i64 " + index + ", " + length);
                    std::string ok = bb(), bad = bb();
                    line("br i1 " + inside + ", label %" + ok + ", label %" + bad);
                    label(bad);
                    line("call void @beans_panic_slice_index(i64 " + index + ", i64 " +
                         length + ", i64 " + std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                    std::string item_pointer = reg(), result = reg();
                    line(item_pointer + " = getelementptr " + std::string(ll(element)) +
                         ", ptr " + pointer + ", i64 " + index);
                    line(result + " = load " + std::string(ll(element)) + ", ptr " +
                         item_pointer + ", align 1");
                    return {result, element};
                }
                if (obj.second->k == Ty::fixed_array_) {
                    Ty* elem = obj.second->args[0];
                    std::string index = to_slot(idx);
                    std::string okc = reg();
                    line(okc + " = icmp ult i64 " + index + ", " +
                         std::to_string(obj.second->array_len));
                    std::string okb = bb(), badb = bb();
                    line("br i1 " + okc + ", label %" + okb + ", label %" + badb);
                    label(badb);
                    line("call void @beans_panic_array_index(i64 " + index + ", i64 " +
                         std::to_string(obj.second->array_len) + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(okb);
                    std::string slot = fresh_slot("arrread", obj.second);
                    line("store " + std::string(ll(obj.second)) + " " + obj.first +
                         ", ptr " + slot);
                    std::string pointer = reg(), result = reg();
                    line(pointer + " = getelementptr " + std::string(ll(obj.second)) +
                         ", ptr " + slot + ", i64 0, i64 " + index);
                    line(result + " = load " + std::string(ll(elem)) + ", ptr " + pointer);
                    return {result, elem};
                }
                if (obj.second->k != Ty::list_) {
                    err(e, "indexing this");
                    return {"0", cg.t_i64()};
                }
                // bounds-checked read
                Ty* elem = obj.second->args[0];
                std::string len = load_at(obj.first, 8, cg.t_i64());
                std::string index = to_slot(idx);
                std::string okc = reg();
                line(okc + " = icmp ult i64 " + index + ", " + len);
                std::string okb = bb(), badb = bb();
                line("br i1 " + okc + ", label %" + okb + ", label %" + badb);
                label(badb);
                line("call void @beans_panic_index(i64 " + index + ", i64 " + len +
                     ", i64 1, i64 " + std::to_string(e->line) + ", i64 " +
                     std::to_string(e->col) + ")");
                line("unreachable");
                label(okb);
                std::string data = load_at(obj.first, 0, cg.t_str()); // ptr
                std::string ep = reg(), raw = reg();
                line(ep + " = getelementptr i64, ptr " + data + ", i64 " + index);
                line(raw + " = load i64, ptr " + ep);
                return {from_slot(raw, elem), elem};
            }
            case Expr::Kind::list_lit: {
                if (hint && hint->k == Ty::fixed_array_) {
                    Ty* elem = hint->args[0];
                    std::string current = "poison";
                    std::string array_type = ll(hint);
                    std::string elem_type = ll(elem);
                    for (size_t i = 0; i < e->args.size(); i++) {
                        EV value = eval(e->args[i].get(), elem);
                        std::string next = reg();
                        line(next + " = insertvalue " + array_type + " " + current +
                             ", " + elem_type + " " + value.first + ", " +
                             std::to_string(i));
                        current = next;
                    }
                    return {current, hint};
                }
                Ty* elem = hint && hint->k == Ty::list_ ? hint->args[0] : nullptr;
                std::vector<EV> elems;
                for (const ExprPtr& el : e->args) {
                    EV v = eval(el.get(), elem);
                    if (!elem) elem = v.second;
                    elems.push_back(std::move(v));
                }
                if (!elem) elem = cg.t_i64();
                std::string l = reg();
                line(l + " = call ptr @beans_list_new(i64 " +
                     std::string(is_slot_rc(elem) ? "1" : "0") + ")");
                for (const EV& v : elems) {
                    transfer_in(v);
                    line("call void @beans_list_push(ptr " + l + ", i64 " +
                         to_slot(v, true) + ")");
                }
                own(l, cg.t_list(elem));
                return {l, cg.t_list(elem)};
            }
            case Expr::Kind::init:
                return eval_init(e, hint);
            case Expr::Kind::cast:
                return eval_cast(e);
            case Expr::Kind::try_: {
                EV v = eval(e->object.get());
                if (v.second->k != Ty::enum_) {
                    err(e, "? here");
                    return {"0", cg.t_i64()};
                }
                Ty* inner = v.second->args.empty() ? cg.t_i64() : v.second->args[0];
                std::string c;
                if (v.second->name == "Option") {
                    c = option_has(v);
                } else {
                    c = result_is_ok(v);
                }
                std::string okb = bb(), errb = bb();
                line("br i1 " + c + ", label %" + okb + ", label %" + errb);
                label(errb);
                if (has_owned_refs(v.second) && !in_temps(v.first))
                    emit_retain_value(v.first, v.second); // caller gets +1
                emit_ret("ret " + std::string(ll(v.second)) + " " + v.first, v.first);
                label(okb);
                std::string payload = v.second->name == "Option"
                                          ? option_payload(v, inner)
                                          : result_payload(v, true);
                if (has_owned_refs(inner)) {
                    emit_retain_value(payload, inner); // survives the enum value
                    own(payload, inner);
                }
                return {payload, inner};
            }
            case Expr::Kind::if_expr: {
                EV c = eval(e->cond.get());
                std::string then_bb = bb(), else_bb = bb(), end_bb = bb();
                line("br i1 " + c.first + ", label %" + then_bb + ", label %" + else_bb);
                label(then_bb);
                size_t tmark = temps.size();
                EV a = eval(e->then_e.get(), hint);
                if (has_owned_refs(a.second)) transfer_in(a);
                std::string slot = fresh_slot("ifv", a.second);
                line("store " + std::string(ll(a.second)) + " " + a.first + ", ptr " + slot);
                flush_temps(tmark);
                line("br label %" + end_bb);
                label(else_bb);
                size_t emark = temps.size();
                EV b2 = eval(e->else_e.get(), hint ? hint : a.second);
                if (has_owned_refs(a.second)) transfer_in(b2);
                line("store " + std::string(ll(a.second)) + " " + b2.first + ", ptr " + slot);
                flush_temps(emark);
                line("br label %" + end_bb);
                label(end_bb);
                std::string r = reg();
                line(r + " = load " + std::string(ll(a.second)) + ", ptr " + slot);
                own(r, a.second);
                return {r, a.second};
            }
            case Expr::Kind::match_expr:
                return eval_match(e, hint);
            case Expr::Kind::closure:
                return eval_closure(e);
            default:
                err(e, "this expression");
                return {"0", cg.t_i64()};
        }
    }

    // lambda-lift: emit the closure as a top-level fn taking its env ptr,
    // and build a box {fnptr, cell...} at the site
    EV eval_closure(const Expr* e) {
        std::vector<std::pair<std::string, Ty*>> caps;
        for (const std::string& name : closure_free_names(e)) {
            if (Var* v = find_var(name)) caps.push_back({name, v->ty});
        }

        std::string sym = "@clo" + std::to_string(cg.next_clo++);
        closure_symbols[e] = sym;
        FnEmit fe(cg, e, sym, &caps, env);
        fe.pkg = pkg; // closure body is code of the same package
        cg.lifted += fe.emit();

        std::vector<Ty*> sig;
        for (const Param& p : e->params) {
            sig.push_back(rt(p.type.get(), e->line, e->col));
        }
        sig.push_back(e->type ? rt(e->type.get(), e->line, e->col) : cg.t_unit());
        Ty* fty = cg.t_fn(std::move(sig));

        long long mask = 0;
        for (size_t i = 0; i < caps.size(); i++) mask |= 1LL << (i + 1);
        std::string box = alloc_bytes(8 + 8 * static_cast<int>(caps.size()),
                                      fixed_meta(mask));
        store_at(box, 0, sym, cg.t_str());
        for (size_t i = 0; i < caps.size(); i++) {
            Var* v = find_var(caps[i].first);
            // v is boxed (the prepass saw this closure) — share the cell, +1
            std::string cell = reg();
            line(cell + " = load ptr, ptr " + v->slot);
            emit_retain(cell);
            store_at(box, 8 + 8 * static_cast<int>(i), cell, cg.t_str());
        }
        own(box, fty);
        return {box, fty};
    }

    // A stored top-level function uses the same box shape and calling
    // convention as a closure. Its tiny adapter ignores the environment word
    // and forwards borrowed arguments to the normal top-level symbol.
    EV eval_top_fn_ref(const Expr* e, const FnDecl* function) {
        if (function->is_extern_c || !function->generics.empty()) {
            err(e, "storing this function");
            return {"null", cg.t_bad()};
        }
        std::vector<Ty*> signature;
        for (const Param& parameter : function->params)
            signature.push_back(
                rt(parameter.type.get(), e->line, e->col));
        Ty* result = function->ret
                         ? rt(function->ret.get(), e->line, e->col)
                         : cg.t_unit();
        signature.push_back(result);
        Ty* function_type = cg.t_fn(std::move(signature));

        std::string adapter;
        auto existing = cg.fn_ref_adapters.find(function);
        if (existing != cg.fn_ref_adapters.end()) {
            adapter = existing->second;
        } else {
            adapter = "@beans_fn_ref_" +
                      std::to_string(cg.fn_ref_adapters.size());
            cg.fn_ref_adapters[function] = adapter;
            std::string body = "define " + std::string(ll(result)) + " " +
                               adapter + "(ptr %env";
            std::string call_arguments;
            for (size_t i = 0; i < function->params.size(); i++) {
                Ty* parameter = function_type->args[i];
                body += ", " + std::string(ll(parameter)) + " %arg" +
                        std::to_string(i);
                if (i) call_arguments += ", ";
                call_arguments += std::string(ll(parameter)) + " %arg" +
                                  std::to_string(i);
            }
            body += ") {\n";
            std::string target = "@b_" + function->qualname;
            if (result->k == Ty::unit_) {
                body += "  call void " + target + "(" + call_arguments +
                        ")\n  ret void\n}\n\n";
            } else {
                body += "  %result = call " + std::string(ll(result)) + " " +
                        target + "(" + call_arguments + ")\n  ret " +
                        std::string(ll(result)) + " %result\n}\n\n";
            }
            cg.lifted += body;
        }

        std::string box = alloc_bytes(8, fixed_meta(0));
        store_at(box, 0, adapter, cg.t_str());
        own(box, function_type);
        return {box, function_type};
    }

    // call a closure/fn value: box layout {fnptr @0, cells @8...}
    EV call_fn_value(const EV& fnv, const Expr* e) {
        Ty* fty = fnv.second;
        std::vector<EV> args;
        for (size_t i = 0; i < e->args.size(); i++) {
            Ty* h = i < fty->fn_nparams() ? fty->args[i] : nullptr;
            args.push_back(eval(e->args[i].get(), h));
        }
        Ty* ret = fty->fn_ret() ? fty->fn_ret() : cg.t_unit();
        std::string fp = load_at(fnv.first, 0, cg.t_str());
        return emit_call(fp, ret, args_text(args, fnv.first));
    }

    EV call_direct_fn_value(const EV& fnv, const Expr* e,
                            const std::string& target) {
        Ty* fty = fnv.second;
        std::vector<EV> args;
        for (size_t i = 0; i < e->args.size(); i++) {
            Ty* hint = i < fty->fn_nparams() ? fty->args[i] : nullptr;
            args.push_back(eval(e->args[i].get(), hint));
        }
        Ty* ret = fty->fn_ret() ? fty->fn_ret() : cg.t_unit();
        return emit_call(target, ret, args_text(args, fnv.first));
    }

    EV dec_literal(std::string_view text) {
        Decimal d = Decimal::parse(clean_number(text));
        return {packed_decimal(d), cg.t_dec()};
    }

    bool decimal_literal_value(const Expr* e, Decimal& out) {
        if (!e || (e->kind != Expr::Kind::int_lit &&
                   e->kind != Expr::Kind::float_lit))
            return false;
        out = Decimal::parse(clean_number(e->text));
        return true;
    }

    int known_decimal_scale(const Expr* e) {
        if (!e) return -1;
        switch (e->kind) {
            case Expr::Kind::int_lit:
            case Expr::Kind::float_lit: {
                Decimal value = Decimal::parse(clean_number(e->text));
                return value.scale;
            }
            case Expr::Kind::ident: {
                Var* var = find_var(std::string(e->text));
                return var && var->ty->k == Ty::dec_ ? var->decimal_scale : -1;
            }
            case Expr::Kind::unary:
                return e->op == TokenKind::minus ? known_decimal_scale(e->rhs.get()) : -1;
            case Expr::Kind::cast: {
                if (!e->type || e->type->name != "decimal") return -1;
                TypeId source = cg.checked_type(e->object.get());
                return source && source->is_int() ? 0 : -1;
            }
            case Expr::Kind::binary: {
                int left = known_decimal_scale(e->lhs.get());
                int right = known_decimal_scale(e->rhs.get());
                if (left < 0 || right < 0) return -1;
                if (e->op == TokenKind::plus || e->op == TokenKind::minus)
                    return std::max(left, right);
                if (e->op == TokenKind::star && left <= 65535 - right)
                    return left + right;
                return -1;
            }
            case Expr::Kind::if_expr: {
                int yes = known_decimal_scale(e->then_e.get());
                int no = known_decimal_scale(e->else_e.get());
                return yes >= 0 && yes == no ? yes : -1;
            }
            default:
                return -1;
        }
    }

    // A fixed-scale add needs no power-of-ten alignment. Decode the signed
    // 112-bit coefficient with two shifts, check its exact bound, then put the
    // scale bits back. This is much smaller than the generic decimal helper.
    EV decimal_literal_add(const EV& lhs, const EV& rhs, Decimal literal,
                           bool subtract, uint32_t line_no, uint32_t col_no,
                           bool scale_is_known = false) {
        const unsigned __int128 coeff_mask =
            (static_cast<unsigned __int128>(1) << 112) - 1;
        const unsigned __int128 scale_mask = ~coeff_mask;
        const __int128 coeff_min =
            -(static_cast<__int128>(1) << 111);
        const __int128 coeff_max =
            (static_cast<__int128>(1) << 111) - 1;
        const __int128 delta = subtract ? -literal.coeff : literal.coeff;

        auto emit_fixed = [&]() -> EV {
            std::string shifted = reg(), coeff = reg();
            line(shifted + " = shl i128 " + lhs.first + ", 16");
            line(coeff + " = ashr i128 " + shifted + ", 16");
            const __int128 limit =
                delta >= 0 ? coeff_max - delta : coeff_min - delta;
            std::string overflow = reg();
            line(overflow + " = icmp " +
                 std::string(delta >= 0 ? "sgt" : "slt") + " i128 " + coeff +
                 ", " + u128_str(static_cast<unsigned __int128>(limit)));
            std::string bad = bb(), okay = bb();
            line("br i1 " + overflow + ", label %" + bad + ", label %" + okay);
            label(bad);
            line("call void @beans_panic(ptr " + cg.intern_string("decimal overflow") +
                 ", i64 " + std::to_string(line_no) + ", i64 " +
                 std::to_string(col_no) + ")");
            line("unreachable");
            label(okay);
            std::string sum = reg();
            line(sum + " = add i128 " + coeff + ", " +
                 u128_str(static_cast<unsigned __int128>(delta)));
            std::string masked = reg();
            line(masked + " = and i128 " + sum + ", " + u128_str(coeff_mask));
            std::string scale_part = reg();
            line(scale_part + " = and i128 " + lhs.first + ", " +
                 u128_str(scale_mask));
            std::string packed = reg();
            line(packed + " = or i128 " + scale_part + ", " + masked);
            return {packed, cg.t_dec()};
        };

        if (scale_is_known) return emit_fixed();

        std::string scale = reg();
        line(scale + " = lshr i128 " + lhs.first + ", 112");
        std::string same_scale = reg();
        line(same_scale + " = icmp eq i128 " + scale + ", " +
             std::to_string(literal.scale));
        std::string fast = bb(), slow = bb(), done = bb();
        std::string result_slot = fresh_slot("decfast", cg.t_dec());
        line("br i1 " + same_scale + ", label %" + fast + ", label %" + slow);

        label(fast);
        EV fixed = emit_fixed();
        line("store i128 " + fixed.first + ", ptr " + result_slot);
        line("br label %" + done);

        label(slow);
        std::string fallback = reg();
        line(fallback + " = call i128 @beans_decv_" +
             std::string(subtract ? "sub" : "add") + "(i128 " + lhs.first +
             ", i128 " + rhs.first + ")");
        line("store i128 " + fallback + ", ptr " + result_slot);
        line("br label %" + done);

        label(done);
        std::string result = reg();
        line(result + " = load i128, ptr " + result_slot);
        return {result, cg.t_dec()};
    }

    EV eval_string(const Expr* e) {
        std::vector<StrPiece> parts = split_interp(e->text, interp_srcs);
        if (parts.empty()) return {cg.intern_string(""), cg.t_str()};
        struct BuildPiece {
            int kind; // runtime: 0 string, 1 signed, 2 unsigned, 3 f64, 4 bool
            std::string type;
            std::string value;
        };
        std::vector<BuildPiece> pieces;
        pieces.reserve(parts.size());
        for (StrPiece& p : parts) {
            std::string piece;
            if (p.expr) {
                EV v = eval(p.expr.get());
                if (!p.spec.has && v.second->k == Ty::i64_) {
                    pieces.push_back(
                        {v.second->is_unsigned ? 2 : 1, "i64", to_slot(v)});
                    continue;
                }
                if (!p.spec.has && v.second->k == Ty::i1_) {
                    std::string z = reg();
                    line(z + " = zext i1 " + v.first + " to i32");
                    pieces.push_back({4, "i32", z});
                    continue;
                }
                if (!p.spec.has && v.second->k == Ty::f64_) {
                    pieces.push_back({3, "double", as_f64(v)});
                    continue;
                }
                if (p.spec.has && p.spec.places >= 0 && v.second->k == Ty::f64_) {
                    std::string fr = reg();
                    line(fr + " = call ptr @beans_fmt_float(double " + as_f64(v) +
                         ", i64 " + std::to_string(p.spec.places) + ")");
                    own(fr, cg.t_str());
                    piece = fr;
                } else if (p.spec.has && p.spec.places >= 0 &&
                           v.second->k == Ty::dec_) {
                    std::string fr = reg();
                    line(fr + " = call ptr @beans_decv_fmt(i128 " + v.first +
                         ", i64 " + std::to_string(p.spec.places) + ")");
                    own(fr, cg.t_str());
                    piece = fr;
                } else {
                    piece = to_str(v, p.expr.get());
                }
                if (p.spec.has && p.spec.width > 0) {
                    std::string pr = reg();
                    line(pr + " = call ptr @beans_fmt_pad_" +
                         (p.spec.left ? "right" : "left") + "(ptr " + piece +
                         ", i64 " + std::to_string(p.spec.width) + ")");
                    own(pr, cg.t_str());
                    piece = pr;
                }
            } else {
                piece = cg.intern_string(p.text);
            }
            pieces.push_back({0, "ptr", piece});
        }
        if (pieces.size() == 1 && pieces[0].kind == 0)
            return {pieces[0].value, cg.t_str()};

        // Interpolation used to concatenate left-to-right. That allocated and
        // copied every prefix, so `"item{x}_{y}"` made three throwaway
        // strings. The runtime now also renders scalar pieces straight into
        // that final allocation, avoiding temporary integer/bool/float strings.
        std::string args;
        for (const BuildPiece& piece : pieces) {
            args += ", i64 " + std::to_string(piece.kind) + ", " + piece.type + " " +
                    piece.value;
        }
        std::string r = reg();
        line(r + " = call ptr (i64, ...) @beans_interpolate(i64 " +
             std::to_string(pieces.size()) + args + ")");
        own(r, cg.t_str());
        return {r, cg.t_str()};
    }

    std::string to_str(const EV& v, const Expr* at) {
        std::string r = reg();
        switch (v.second->k) {
            case Ty::str_: return v.first;
            case Ty::i64_:
                line(r + " = call ptr @beans_from_" +
                     std::string(v.second->is_unsigned ? "uint" : "int") +
                     "(i64 " + to_slot(v) + ")");
                own(r, cg.t_str());
                return r;
            case Ty::f64_:
                line(r + " = call ptr @beans_from_float(double " + as_f64(v) + ")");
                own(r, cg.t_str());
                return r;
            case Ty::dec_:
                line(r + " = call ptr @beans_decv_str(i128 " + v.first + ")");
                own(r, cg.t_str());
                return r;
            case Ty::i1_: {
                std::string z = reg();
                line(z + " = zext i1 " + v.first + " to i32");
                line(r + " = call ptr @beans_from_bool(i32 " + z + ")");
                own(r, cg.t_str());
                return r;
            }
            case Ty::list_: {
                std::string es = cg.request_show(v.second->args[0]);
                line(r + " = call ptr @beans_show_list(ptr " + v.first + ", ptr " +
                     es + ")");
                own(r, cg.t_str());
                return r;
            }
            case Ty::enum_: {
                std::string sym = cg.request_show(v.second);
                std::string s = reg();
                line(s + " = ptrtoint ptr " + v.first + " to i64");
                line(r + " = call ptr " + sym + "(i64 " + s + ")");
                own(r, cg.t_str());
                return r;
            }
            default:
                err(at, "printing this value");
                return cg.intern_string("?");
        }
    }

    std::string inline_equal(const std::string& left, const std::string& right,
                             Ty* type) {
        if (type->k == Ty::f64_) {
            std::string same = reg();
            line(same + " = fcmp oeq " + std::string(ll(type)) + " " + left + ", " +
                 right);
            return same;
        }
        if (type->k == Ty::simd4f32_) {
            std::string all = "true";
            for (int i = 0; i < 4; i++) {
                std::string l = reg(), r = reg(), same = reg();
                line(l + " = extractelement <4 x float> " + left + ", i32 " +
                     std::to_string(i));
                line(r + " = extractelement <4 x float> " + right + ", i32 " +
                     std::to_string(i));
                line(same + " = fcmp oeq float " + l + ", " + r);
                if (all == "true") all = same;
                else {
                    std::string combined = reg();
                    line(combined + " = and i1 " + all + ", " + same);
                    all = combined;
                }
            }
            return all;
        }
        if (type->k == Ty::fixed_array_) {
            std::string all = "true";
            for (uint32_t i = 0; i < type->array_len; i++) {
                std::string l = reg(), r = reg();
                line(l + " = extractvalue " + std::string(ll(type)) + " " + left + ", " +
                     std::to_string(i));
                line(r + " = extractvalue " + std::string(ll(type)) + " " + right + ", " +
                     std::to_string(i));
                std::string same = inline_equal(l, r, type->args[0]);
                if (all == "true") all = same;
                else {
                    std::string combined = reg();
                    line(combined + " = and i1 " + all + ", " + same);
                    all = combined;
                }
            }
            return all;
        }
        if (type->k == Ty::struct_) {
            std::string all = "true";
            auto it = cg.impl_by_name.find(type->name);
            if (it == cg.impl_by_name.end() || it->second->decl->is_union) return "false";
            for (size_t i = 0; i < it->second->fields.size(); i++) {
                Ty* field = it->second->fields[i].ty;
                std::string l = reg(), r = reg();
                line(l + " = extractvalue " + std::string(ll(type)) + " " + left + ", " +
                     std::to_string(i));
                line(r + " = extractvalue " + std::string(ll(type)) + " " + right + ", " +
                     std::to_string(i));
                std::string same = inline_equal(l, r, field);
                if (all == "true") all = same;
                else {
                    std::string combined = reg();
                    line(combined + " = and i1 " + all + ", " + same);
                    all = combined;
                }
            }
            return all;
        }
        if (is_inline_option(type)) {
            std::string left_has = reg(), right_has = reg();
            line(left_has + " = extractvalue " + std::string(ll(type)) + " " + left +
                 ", 0");
            line(right_has + " = extractvalue " + std::string(ll(type)) + " " + right +
                 ", 0");
            std::string tags_same = reg();
            line(tags_same + " = icmp eq i1 " + left_has + ", " + right_has);
            std::string left_payload = reg(), right_payload = reg();
            line(left_payload + " = extractvalue " + std::string(ll(type)) + " " + left +
                 ", 1");
            line(right_payload + " = extractvalue " + std::string(ll(type)) + " " +
                 right + ", 1");
            std::string payload_same =
                inline_equal(left_payload, right_payload, type->args[0]);
            std::string value_same = reg();
            line(value_same + " = select i1 " + left_has + ", i1 " + payload_same +
                 ", i1 true");
            std::string result = reg();
            line(result + " = and i1 " + tags_same + ", " + value_same);
            return result;
        }
        if (is_inline_result(type)) {
            std::string left_error = reg(), right_error = reg();
            line(left_error + " = extractvalue " + std::string(ll(type)) + " " + left +
                 ", 0");
            line(right_error + " = extractvalue " + std::string(ll(type)) + " " +
                 right + ", 0");
            std::string tags_same = reg();
            line(tags_same + " = icmp eq i1 " + left_error + ", " + right_error);
            std::string left_ok = reg(), right_ok = reg();
            std::string left_err = reg(), right_err = reg();
            line(left_ok + " = extractvalue " + std::string(ll(type)) + " " + left +
                 ", 1");
            line(right_ok + " = extractvalue " + std::string(ll(type)) + " " + right +
                 ", 1");
            line(left_err + " = extractvalue " + std::string(ll(type)) + " " + left +
                 ", 2");
            line(right_err + " = extractvalue " + std::string(ll(type)) + " " + right +
                 ", 2");
            std::string ok_same = inline_equal(left_ok, right_ok, type->args[0]);
            std::string err_same = inline_equal(left_err, right_err, type->args[1]);
            std::string payload_same = reg();
            line(payload_same + " = select i1 " + left_error + ", i1 " + err_same +
                 ", i1 " + ok_same);
            std::string result = reg();
            line(result + " = and i1 " + tags_same + ", " + payload_same);
            return result;
        }
        std::string same = reg();
        line(same + " = icmp eq " + std::string(ll(type)) + " " + left + ", " + right);
        return same;
    }

    EV eval_binary(const Expr* e) {
        if (e->op == TokenKind::andand || e->op == TokenKind::oror) {
            bool is_and = e->op == TokenKind::andand;
            std::string slot = fresh_slot("sc", cg.t_bool());
            EV l = eval(e->lhs.get());
            line("store i1 " + l.first + ", ptr " + slot);
            std::string more = bb(), end = bb();
            if (is_and) line("br i1 " + l.first + ", label %" + more + ", label %" + end);
            else line("br i1 " + l.first + ", label %" + end + ", label %" + more);
            label(more);
            size_t smark = temps.size();
            EV r2 = eval(e->rhs.get());
            line("store i1 " + r2.first + ", ptr " + slot);
            flush_temps(smark);
            line("br label %" + end);
            label(end);
            std::string r = reg();
            line(r + " = load i1, ptr " + slot);
            return {r, cg.t_bool()};
        }

        EV l = eval(e->lhs.get());
        EV r2 = eval(e->rhs.get(), l.second);
        if (l.second != r2.second &&
            (e->lhs->kind == Expr::Kind::int_lit || e->lhs->kind == Expr::Kind::float_lit)) {
            l = eval(e->lhs.get(), r2.second);
        }
        std::string r = reg();
        TokenKind op = e->op;

        auto cmp_result = [&](const std::string& c) -> EV {
            const char* pred = nullptr;
            switch (op) {
                case TokenKind::eq: pred = "eq"; break;
                case TokenKind::neq: pred = "ne"; break;
                case TokenKind::lt: pred = "slt"; break;
                case TokenKind::le: pred = "sle"; break;
                case TokenKind::gt: pred = "sgt"; break;
                case TokenKind::ge: pred = "sge"; break;
                default: return {"0", cg.t_bool()};
            }
            line(r + " = icmp " + pred + " i32 " + c + ", 0");
            return {r, cg.t_bool()};
        };

        if (l.second->k == Ty::str_) {
            std::string c = reg();
            line(c + " = call i32 @beans_str_cmp(ptr " + l.first + ", ptr " + r2.first + ")");
            return cmp_result(c);
        }
        if (l.second->k == Ty::bytes_) {
            std::string c = reg();
            line(c + " = call i64 @beans_bytes_eq(ptr " + l.first + ", ptr " + r2.first + ")");
            line(r + " = icmp " + (op == TokenKind::eq ? "ne" : "eq") + " i64 " + c + ", 0");
            return {r, cg.t_bool()};
        }
        if (l.second->k == Ty::fixed_array_) {
            std::string all = inline_equal(l.first, r2.first, l.second);
            if (op == TokenKind::neq) {
                line(r + " = xor i1 " + all + ", 1");
                return {r, cg.t_bool()};
            }
            return {all, cg.t_bool()};
        }
        if (l.second->k == Ty::struct_) {
            std::string all = inline_equal(l.first, r2.first, l.second);
            if (op == TokenKind::neq) {
                line(r + " = xor i1 " + all + ", 1");
                return {r, cg.t_bool()};
            }
            return {all, cg.t_bool()};
        }
        if (l.second->k == Ty::dec_) {
            switch (op) {
                case TokenKind::plus:
                case TokenKind::minus:
                case TokenKind::star:
                case TokenKind::slash: {
                    Decimal literal;
                    if ((op == TokenKind::plus || op == TokenKind::minus) &&
                        decimal_literal_value(e->rhs.get(), literal))
                        return decimal_literal_add(l, r2, literal,
                                                   op == TokenKind::minus,
                                                   e->line, e->col,
                                                   known_decimal_scale(e->lhs.get()) ==
                                                       literal.scale);
                    const char* fn = op == TokenKind::plus    ? "add"
                                     : op == TokenKind::minus ? "sub"
                                     : op == TokenKind::star  ? "mul"
                                                              : "div";
                    if (op == TokenKind::slash) {
                        // div panics on zero; it needs the source position so
                        // the message matches the interpreter's
                        line(r + " = call i128 @beans_decv_div(i128 " + l.first +
                             ", i128 " + r2.first + ", i64 " + std::to_string(e->line) +
                             ", i64 " + std::to_string(e->col) + ")");
                    } else {
                        line(r + " = call i128 @beans_decv_" + fn + "(i128 " + l.first +
                             ", i128 " + r2.first + ")");
                    }
                    return {r, cg.t_dec()};
                }
                default: {
                    std::string c = reg();
                    line(c + " = call i32 @beans_decv_cmp(i128 " + l.first +
                         ", i128 " + r2.first + ")");
                    return cmp_result(c);
                }
            }
        }
        if (l.second->k == Ty::enum_) {
            if (is_inline_option(l.second) || is_inline_result(l.second)) {
                std::string same = inline_equal(l.first, r2.first, l.second);
                if (op == TokenKind::neq) {
                    line(r + " = xor i1 " + same + ", true");
                    return {r, cg.t_bool()};
                }
                return {same, cg.t_bool()};
            }
            // structural equality like the interpreter's value_eq: tags first,
            // then payload fields, deep. Payload-free enums keep the plain tag
            // compare — same answer, no call.
            bool payload = false;
            if (l.second->name == "Option") {
                payload = true;
            } else if (const EnumDecl* d = cg.enum_decls.count(l.second->name)
                                               ? cg.enum_decls[l.second->name]
                                               : nullptr) {
                for (const EnumVariant& v : d->variants) payload |= !v.payload.empty();
            }
            if (!payload) {
                std::string ta = load_at(l.first, 0, cg.t_i64());
                std::string tb = load_at(r2.first, 0, cg.t_i64());
                line(r + " = icmp " + (op == TokenKind::eq ? "eq" : "ne") + " i64 " +
                     ta + ", " + tb);
                return {r, cg.t_bool()};
            }
            std::string sym = cg.request_eq(l.second);
            std::string pa = reg(), pb = reg(), c = reg();
            line(pa + " = ptrtoint ptr " + l.first + " to i64");
            line(pb + " = ptrtoint ptr " + r2.first + " to i64");
            line(c + " = call i64 " + sym + "(i64 " + pa + ", i64 " + pb + ")");
            line(r + " = icmp " + (op == TokenKind::eq ? "ne" : "eq") + " i64 " + c +
                 ", 0");
            return {r, cg.t_bool()};
        }

        bool flt = l.second->k == Ty::f64_ || l.second->k == Ty::simd4f32_;
        auto arith = [&](const char* iop, const char* fop) -> EV {
            line(r + " = " + (flt ? fop : iop) + " " + std::string(ll(l.second)) + " " +
                 l.first + ", " + r2.first);
            r = flt ? normalize_float(r, l.second) : normalize_integer(r, l.second);
            return {r, l.second};
        };
        auto compare = [&](const char* ipred, const char* fpred) -> EV {
            if (flt) line(r + " = fcmp " + std::string(fpred) + " " +
                          std::string(ll(l.second)) + " " + l.first + ", " + r2.first);
            else if (l.second->k == Ty::i1_)
                line(r + " = icmp " + std::string(ipred) + " i1 " + l.first + ", " + r2.first);
            else line(r + " = icmp " + std::string(ipred) + " " +
                      std::string(ll(l.second)) + " " + l.first + ", " + r2.first);
            return {r, cg.t_bool()};
        };
        switch (op) {
            case TokenKind::plus: return arith("add", "fadd");
            case TokenKind::minus: return arith("sub", "fsub");
            case TokenKind::star: return arith("mul", "fmul");
            case TokenKind::slash:
                if (!flt) guard_div_zero(r2.first, l.second, false, e->line, e->col);
                return arith(l.second->is_unsigned ? "udiv" : "sdiv", "fdiv");
            case TokenKind::percent:
                if (!flt) guard_div_zero(r2.first, l.second, true, e->line, e->col);
                return arith(l.second->is_unsigned ? "urem" : "srem", "frem");
            case TokenKind::shl: {
                std::string amount = reg();
                line(amount + " = and " + std::string(ll(l.second)) + " " + r2.first +
                     ", " + std::to_string((l.second->bits ? l.second->bits : 64) - 1));
                r2.first = amount;
                return arith("shl", "shl");
            }
            case TokenKind::shr: {
                std::string amount = reg();
                line(amount + " = and " + std::string(ll(l.second)) + " " + r2.first +
                     ", " + std::to_string((l.second->bits ? l.second->bits : 64) - 1));
                r2.first = amount;
                return arith(l.second->is_unsigned ? "lshr" : "ashr", "ashr");
            }
            case TokenKind::amp: return arith("and", "and");
            case TokenKind::pipe: return arith("or", "or");
            case TokenKind::caret: return arith("xor", "xor");
            case TokenKind::eq: return compare("eq", "oeq");
            case TokenKind::neq: return compare("ne", "one");
            case TokenKind::lt: return compare(l.second->is_unsigned ? "ult" : "slt", "olt");
            case TokenKind::le: return compare(l.second->is_unsigned ? "ule" : "sle", "ole");
            case TokenKind::gt: return compare(l.second->is_unsigned ? "ugt" : "sgt", "ogt");
            case TokenKind::ge: return compare(l.second->is_unsigned ? "uge" : "sge", "oge");
            default:
                err(e, "this operator");
                return {"0", cg.t_i64()};
        }
    }

    EV eval_field(const Expr* e) {
        const Expr* obj = e->object.get();
        if (obj->kind == Expr::Kind::ident && !find_var(std::string(obj->text))) {
            // bare enum variant value: Payment.cash
            std::string n = obj->resolved.empty() ? cg.qual(std::string(obj->text))
                                                  : obj->resolved;
            if (cg.enum_decls.count(n)) {
                int tag = cg.variant_tag(n, e->name);
                std::string b = box_enum(tag, {});
                return {b, cg.t_enum(n, {})};
            }
        }
        if (obj->kind == Expr::Kind::field && !obj->resolved.empty() &&
            cg.enum_decls.count(obj->resolved)) {
            // qualified bare variant: util.Status.active
            int tag = cg.variant_tag(obj->resolved, e->name);
            std::string b = box_enum(tag, {});
            return {b, cg.t_enum(obj->resolved, {})};
        }
        EV v = eval(obj);
        if (v.second->k == Ty::struct_) {
            auto it = cg.impl_by_name.find(v.second->name);
            if (it != cg.impl_by_name.end()) {
                for (size_t i = 0; i < it->second->fields.size(); i++) {
                    const CImpl::FieldInfo& field = it->second->fields[i];
                    if (field.name != e->name) continue;
                    if (it->second->decl->is_union) {
                        std::string slot = fresh_slot("unionread", v.second);
                        line("store " + std::string(ll(v.second)) + " " + v.first +
                             ", ptr " + slot);
                        std::string result = reg();
                        line(result + " = load " + std::string(ll(field.ty)) +
                             ", ptr " + slot);
                        return {result, field.ty};
                    }
                    std::string result = reg();
                    line(result + " = extractvalue " + std::string(ll(v.second)) + " " +
                         v.first + ", " + std::to_string(i));
                    return {result, field.ty};
                }
            }
        }
        if (v.second->k == Ty::obj_) {
            if (v.second->name == "Error") {
                int off = e->name == "msg" ? 16 : 24;
                return {load_at(v.first, off, cg.t_str()), cg.t_str()};
            }
            auto it = cg.impl_by_name.find(v.second->name);
            if (it != cg.impl_by_name.end()) {
                for (const CImpl::FieldInfo& f : it->second->fields) {
                    if (f.name == e->name) {
                        return {load_at(v.first, f.offset, f.ty), f.ty};
                    }
                }
            }
        }
        err(e, "field '" + e->name + "'");
        return {"0", cg.t_i64()};
    }

    // exit path: release live temps (keeping the returned value), run armed
    // defers (newest first), release frame-owned locals, then return.
    void emit_ret(const std::string& ret_instr, const std::string& except = "",
                  const std::string& moved_slot = "") {
        for (const EV& t : temps) {
            if (t.first != except) emit_release_value(t.first, t.second);
        }
        for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
            std::string flag = reg();
            line(flag + " = load i1, ptr " + it->flag);
            std::string runb = bb(), skipb = bb();
            line("br i1 " + flag + ", label %" + runb + ", label %" + skipb);
            label(runb);
            std::vector<std::map<std::string, Var>> saved = std::move(scopes);
            scopes = it->scope_snap;
            size_t dmark = temps.size();
            eval(it->expr);
            flush_temps(dmark);
            scopes = std::move(saved);
            line("br label %" + skipb);
            label(skipb);
        }
        // A direct `return local` can hand the local's frame-owned reference
        // to the caller. Defers above still saw the live local; only the final
        // frame drop is skipped. This is the first MIR last-use ARC fold.
        release_scopes(0, moved_slot);
        if (!deinit_chain.empty()) {
            // parent's deinit after this one is fully done (defers included),
            // while self is still alive — the caller walks the children next
            Var* sv = find_var("self");
            if (sv) line("call void " + deinit_chain + "(ptr " + var_read(sv) + ")");
        }
        line(ret_instr);
    }
    bool in_temps(const std::string& v) const {
        for (const EV& t : temps) {
            if (t.first == v) return true;
        }
        return false;
    }

    void exec_index_assign(const Stmt* s) {
        EV obj = eval(s->target->object.get());
        if (obj.second->k == Ty::fixed_array_) {
            if (s->target->object->kind != Expr::Kind::ident) {
                cg.err(s->line, s->col, "assigning into this fixed array");
                return;
            }
            Var* owner = find_var(std::string(s->target->object->text));
            if (!owner) {
                cg.err(s->line, s->col, "finding this fixed array local");
                return;
            }
            Ty* elem = obj.second->args[0];
            EV idx = eval(s->target->index_expr.get(), cg.t_i64());
            std::string index = to_slot(idx), okc = reg();
            line(okc + " = icmp ult i64 " + index + ", " +
                 std::to_string(obj.second->array_len));
            std::string okb = bb(), badb = bb();
            line("br i1 " + okc + ", label %" + okb + ", label %" + badb);
            label(badb);
            line("call void @beans_panic_array_index(i64 " + index + ", i64 " +
                 std::to_string(obj.second->array_len) + ", i64 " +
                 std::to_string(s->line) + ", i64 " + std::to_string(s->col) + ")");
            line("unreachable");
            label(okb);
            std::string pointer = reg();
            line(pointer + " = getelementptr " + std::string(ll(obj.second)) +
                 ", ptr " + var_ptr(owner) + ", i64 0, i64 " + index);
            EV value = eval(s->value.get(), elem);
            if (s->op == TokenKind::assign) {
                line("store " + std::string(ll(elem)) + " " + value.first +
                     ", ptr " + pointer);
                return;
            }
            bool floating = elem->k == Ty::f64_;
            const char* operation = nullptr;
            switch (s->op) {
                case TokenKind::plus_eq: operation = floating ? "fadd" : "add"; break;
                case TokenKind::minus_eq: operation = floating ? "fsub" : "sub"; break;
                case TokenKind::star_eq: operation = floating ? "fmul" : "mul"; break;
                case TokenKind::slash_eq:
                    operation = floating ? "fdiv" : elem->is_unsigned ? "udiv" : "sdiv";
                    break;
                case TokenKind::percent_eq:
                    operation = floating ? "frem" : elem->is_unsigned ? "urem" : "srem";
                    break;
                default: return;
            }
            if (!floating && (s->op == TokenKind::slash_eq ||
                              s->op == TokenKind::percent_eq)) {
                guard_div_zero(value.first, elem, s->op == TokenKind::percent_eq,
                               s->value->line, s->value->col);
            }
            std::string current = reg(), result = reg();
            line(current + " = load " + std::string(ll(elem)) + ", ptr " + pointer);
            line(result + " = " + operation + " " + std::string(ll(elem)) + " " +
                 current + ", " + value.first);
            line("store " + std::string(ll(elem)) + " " + result + ", ptr " + pointer);
            return;
        }
        if (obj.second->k == Ty::list_) {
            Ty* elem = obj.second->args[0];
            EV idx = eval(s->target->index_expr.get());
            std::string len = load_at(obj.first, 8, cg.t_i64());
            std::string index = to_slot(idx);
            std::string okc = reg();
            line(okc + " = icmp ult i64 " + index + ", " + len);
            std::string okb = bb(), badb = bb();
            line("br i1 " + okc + ", label %" + okb + ", label %" + badb);
            label(badb);
            line("call void @beans_panic_index(i64 " + index + ", i64 " + len +
                 ", i64 0, i64 " + std::to_string(s->line) + ", i64 " +
                 std::to_string(s->col) + ")");
            line("unreachable");
            label(okb);
            EV v = eval(s->value.get(), elem);
            transfer_in(v);
            std::string data = load_at(obj.first, 0, cg.t_str());
            std::string ep = reg();
            line(ep + " = getelementptr i64, ptr " + data + ", i64 " + index);
            if (is_slot_rc(elem)) {
                std::string oldraw = reg(), oldp = reg();
                line(oldraw + " = load i64, ptr " + ep);
                line(oldp + " = inttoptr i64 " + oldraw + " to ptr");
                line("store i64 " + to_slot(v, true) + ", ptr " + ep);
                emit_release(oldp);
            } else {
                line("store i64 " + to_slot(v, true) + ", ptr " + ep);
            }
            return;
        }
        if (obj.second->k == Ty::map_) {
            Ty* K = obj.second->args[0];
            Ty* V = obj.second->args[1];
            int kind = eq_kind(K);
            EV k = eval(s->target->index_expr.get(), K);
            EV v = eval(s->value.get(), V);
            // the map owns both refs — storing borrows here corrupted the heap
            transfer_in(k);
            transfer_in(v);
            if (kind == 0) {
                line("call void @beans_map_set_raw(ptr " + obj.first + ", i64 " +
                     to_slot(k, true) + ", i64 " + to_slot(v, true) + ")");
            } else {
                line("call void @beans_map_set(ptr " + obj.first + ", i64 " +
                     to_slot(k, true) + ", i64 " + to_slot(v, true) + ", i64 " +
                     std::to_string(kind) + ", ptr " + eq_thunk(K, kind) + ", ptr " +
                     hash_thunk(K, kind) + ")");
            }
            return;
        }
        cg.err(s->line, s->col, "assigning into this");
    }

    // where a field lives, for assignment
    std::pair<std::string, Ty*> field_ptr(const Expr* e) {
        EV v = eval(e->object.get());
        if (v.second->k == Ty::struct_ && e->object->kind == Expr::Kind::ident) {
            Var* owner = find_var(std::string(e->object->text));
            auto it = cg.impl_by_name.find(v.second->name);
            if (owner && it != cg.impl_by_name.end()) {
                for (size_t i = 0; i < it->second->fields.size(); i++) {
                    const CImpl::FieldInfo& field = it->second->fields[i];
                    if (field.name != e->name) continue;
                    if (it->second->decl->is_union)
                        return {var_ptr(owner), field.ty};
                    std::string pointer = reg();
                    line(pointer + " = getelementptr " + std::string(ll(v.second)) +
                         ", ptr " + var_ptr(owner) + ", i64 0, i32 " +
                         std::to_string(i));
                    return {pointer, field.ty};
                }
            }
        }
        if (v.second->k == Ty::obj_) {
            auto it = cg.impl_by_name.find(v.second->name);
            if (it != cg.impl_by_name.end()) {
                for (const CImpl::FieldInfo& f : it->second->fields) {
                    if (f.name == e->name) {
                        std::string p = reg();
                        line(p + " = getelementptr i8, ptr " + v.first + ", i64 " +
                             std::to_string(f.offset));
                        return {p, f.ty};
                    }
                }
            }
        }
        err(e, "assigning to this field");
        return {"null", cg.t_i64()};
    }

    EV eval_cast(const Expr* e) {
        EV v = eval(e->object.get());
        Ty* to = rt(e->type.get(), e->line, e->col);
        if (e->checked) {
            // as? — runtime class check via the parents table
            if (v.second->k != Ty::obj_ || to->k != Ty::obj_) {
                err(e, "this as?");
                return {"null", cg.t_bad()};
            }
            auto it = cg.impl_by_name.find(to->name);
            if (it == cg.impl_by_name.end()) {
                err(e, "as? to an interface");
                return {"null", cg.t_bad()};
            }
            std::string descriptor = load_at(v.first, 0, cg.t_str());
            std::string id = load_at(descriptor, 0, cg.t_i64());
            std::string c = reg(), cb = reg();
            line(c + " = call i64 @beans_is_a(i64 " + id + ", i64 " +
                 std::to_string(it->second->id) + ")");
            line(cb + " = icmp ne i64 " + c + ", 0");
            std::string yes = bb(), no = bb(), end = bb();
            std::string slot = fresh_slot("asq", cg.t_str()); // ptr slot
            line("br i1 " + cb + ", label %" + yes + ", label %" + no);
            label(yes);
            std::string sb = make_option_some({v.first, to}, to);
            consume(sb);
            line("store ptr " + sb + ", ptr " + slot);
            line("br label %" + end);
            label(no);
            std::string nb = make_option_none(to);
            consume(nb);
            line("store ptr " + nb + ", ptr " + slot);
            line("br label %" + end);
            label(end);
            std::string r = reg();
            line(r + " = load ptr, ptr " + slot);
            own(r, cg.t_option(to));
            return {r, cg.t_option(to)};
        }
        if (v.second == to) return v;
        std::string r = reg();
        if (v.second->k == Ty::i64_ && to->k == Ty::f64_) {
            line(r + " = " + std::string(v.second->is_unsigned ? "uitofp" : "sitofp") +
                 " " + std::string(ll(v.second)) + " " + v.first + " to " +
                 std::string(ll(to)));
            return {r, to};
        }
        if (v.second->k == Ty::f64_ && to->k == Ty::i64_) {
            line(r + " = " + std::string(to->is_unsigned ? "fptoui" : "fptosi") +
                 " " + std::string(ll(v.second)) + " " + v.first + " to " +
                 std::string(ll(to)));
            return {r, to};
        }
        if (v.second->k == Ty::i64_ && to->k == Ty::dec_) {
            line(r + " = call i128 @beans_decv_from_int(i64 " + to_slot(v) + ")");
            return {r, to};
        }
        if (v.second->k == Ty::dec_ && to->k == Ty::i64_) {
            std::string raw = reg();
            line(raw + " = call i64 @beans_decv_to_int(i128 " + v.first + ")");
            return {from_slot(raw, to), to};
        }
        if (v.second->k == Ty::f64_ && to->k == Ty::dec_) {
            std::string number = v.first;
            if (v.second->bits == 32) {
                number = reg();
                line(number + " = fpext float " + v.first + " to double");
            }
            line(r + " = call i128 @beans_decv_from_f64(double " + number + ")");
            return {r, to};
        }
        if (v.second->k == Ty::dec_ && to->k == Ty::f64_) {
            std::string raw = reg();
            line(raw + " = call double @beans_decv_to_f64(i128 " + v.first + ")");
            if (to->bits == 32) {
                line(r + " = fptrunc double " + raw + " to float");
                return {r, to};
            }
            return {raw, to};
        }
        if (v.second->k == Ty::i64_ && to->k == Ty::i64_) {
            if (v.second->bits == to->bits) return {v.first, to};
            line(r + " = " +
                 std::string(v.second->bits > to->bits
                                 ? "trunc"
                                 : v.second->is_unsigned ? "zext" : "sext") +
                 " " + std::string(ll(v.second)) + " " + v.first + " to " +
                 std::string(ll(to)));
            return {r, to};
        }
        if (v.second->k == Ty::f64_ && to->k == Ty::f64_) {
            if (v.second->bits == to->bits) return {v.first, to};
            line(r + " = " + std::string(v.second->bits > to->bits ? "fptrunc" : "fpext") +
                 " " + std::string(ll(v.second)) + " " + v.first + " to " +
                 std::string(ll(to)));
            return {r, to};
        }
        if (v.second->k == Ty::obj_ && to->k == Ty::obj_) return {v.first, to}; // upcast
        err(e, "this cast");
        return {v.first, to};
    }

    static bool impl_chain_has_deinit(CImpl* im) {
        for (CImpl* p = im; p; p = p->parent) {
            for (const FnDecl& m : p->decl->methods) {
                if (m.has_self && m.name == "deinit" && m.has_body) return true;
            }
        }
        return false;
    }

    // flag the rc word of a freshly built object whose chain has a deinit —
    // every construction path must do this (ctor call and raw initializer),
    // or the object dies silently
    void emit_fin_flag(const std::string& o, CImpl* im) {
        if (!impl_chain_has_deinit(im)) return;
        std::string rcp = reg(), rcv = reg(), rcn = reg();
        line(rcp + " = getelementptr i8, ptr " + o + ", i64 -16");
        line(rcv + " = load i64, ptr " + rcp);
        line(rcn + " = or i64 " + rcv + ", " + std::to_string(1LL << 61));
        line("store i64 " + rcn + ", ptr " + rcp);
    }

    // ClassName(args): fresh object — defaults in, the rest zero, which the
    // checker proved init assigns before anything reads — then the init call.
    // Args are evaluated before the allocation, matching the interpreter.
    EV eval_ctor(const Expr* e, const ClassDecl* cd, Ty* hint) {
        CImpl* im = nullptr;
        if (cd->generics.empty()) {
            im = cg.request_impl(cd, {}, e->line, e->col);
        } else if (hint && (hint->k == Ty::obj_ || hint->k == Ty::struct_)) {
            auto it = cg.impl_by_name.find(hint->name);
            if (it != cg.impl_by_name.end()) im = it->second;
        }
        // the constructor may be inherited — nearest impl up the chain that
        // declares an init builds this class
        const FnDecl* ini = nullptr;
        CImpl* owner = nullptr;
        for (CImpl* p = im; p && !ini; p = p->parent) {
            for (const FnDecl& m : p->decl->methods) {
                if (m.has_self && m.name == "init") { ini = &m; owner = p; }
            }
        }
        if (!im || !ini) {
            err(e, "building this");
            return {"null", cg.t_bad()};
        }

        std::vector<EV> args = eval_args(e, ini->params, owner->env);

        long long mask = 0;
        for (const CImpl::FieldInfo& f : im->fields) {
            if (is_rc(f.ty)) mask |= 1LL << (f.offset / 8);
        }
        std::string o = alloc_bytes(im->size, fixed_meta(mask));
        line("store ptr @td_" + im->mangled + ", ptr " + o);
        for (const CImpl::FieldInfo& f : im->fields) {
            if (f.decl->def) {
                EV v = eval(f.decl->def.get(), f.ty);
                transfer_in(v);
                store_at(o, f.offset, v.first, f.ty);
            } else if (f.ty->k == Ty::f64_) {
                store_at(o, f.offset, fmt_double(0), f.ty);
            } else if (f.ty->k == Ty::i64_ || f.ty->k == Ty::i1_ ||
                       f.ty->k == Ty::dec_) {
                store_at(o, f.offset, "0", f.ty);
            } else {
                store_at(o, f.offset, "null", f.ty);
            }
        }
        emit_fin_flag(o, im);
        emit_call("@m_" + owner->mangled + "_init", cg.t_unit(),
                  args_text(args, o, &ini->params));
        own(o, cg.t_obj(im->mangled));
        return {o, cg.t_obj(im->mangled)};
    }

    EV eval_init(const Expr* e, Ty* hint) {
        // map literal / short map init
        bool is_map_hint = hint && hint->k == Ty::map_;
        bool has_expr_keys = false;
        for (const InitEntry& en : e->entries) has_expr_keys |= en.key != nullptr;
        if (e->name.empty() && (is_map_hint || has_expr_keys)) {
            Ty* K = is_map_hint ? hint->args[0] : cg.t_str();
            Ty* V = is_map_hint ? hint->args[1] : cg.t_i64();
            int kind = eq_kind(K);
            std::string m = reg();
            bool ordered = is_map_hint && hint->name == "OrderedMap";
            line(m + " = call ptr @beans_map_new(i64 " +
                 std::string(is_slot_rc(K) ? "1" : "0") + ", i64 " +
                 std::string(is_slot_rc(V) ? "1" : "0") + ", i64 " +
                 (ordered ? "1" : "0") + ")");
            for (const InitEntry& en : e->entries) {
                EV k = en.key ? eval(en.key.get(), K)
                              : EV{cg.intern_string(en.name), cg.t_str()};
                EV v = eval(en.value.get(), V);
                transfer_in(k);
                transfer_in(v);
                if (kind == 0) {
                    line("call void @beans_map_set_raw(ptr " + m + ", i64 " +
                         to_slot(k, true) + ", i64 " + to_slot(v, true) + ")");
                } else {
                    line("call void @beans_map_set(ptr " + m + ", i64 " +
                         to_slot(k, true) + ", i64 " + to_slot(v, true) +
                         ", i64 " + std::to_string(kind) +
                         ", ptr " + eq_thunk(K, kind) + ", ptr " +
                         hash_thunk(K, kind) + ")");
                }
            }
            Ty* map_type = cg.t_map(K, V, ordered);
            own(m, map_type);
            return {m, map_type};
        }

        CImpl* im = nullptr;
        const ClassDecl* cd = nullptr;
        if (!e->name.empty() || !e->resolved.empty()) {
            std::string key = e->resolved.empty() ? cg.qual(e->name) : e->resolved;
            auto cit = cg.class_decls.find(key);
            if (cit != cg.class_decls.end()) cd = cit->second;
        }
        if (cd && e->type_args.size() == cd->generics.size()) {
            // non-generic, or generic with explicit args — instantiate directly
            std::vector<Ty*> targs;
            for (const TypePtr& t : e->type_args)
                targs.push_back(rt(t.get(), e->line, e->col));
            im = cg.request_impl(cd, std::move(targs), e->line, e->col);
        } else if (hint && (hint->k == Ty::obj_ || hint->k == Ty::struct_)) {
            // short init / generic with elided args: the declared type's impl
            auto it = cg.impl_by_name.find(hint->name);
            if (it != cg.impl_by_name.end()) im = it->second;
        }
        if (!im) {
            err(e, "building this");
            return {"null", cg.t_bad()};
        }
        if (im->decl->is_struct || im->decl->is_union) {
            Ty* result_ty = cg.t_struct(im->mangled,
                                        static_cast<uint32_t>(im->size),
                                        static_cast<uint32_t>(im->align));
            if (im->decl->is_union) {
                std::string slot = fresh_slot("unioninit", result_ty);
                line("store " + std::string(ll(result_ty)) +
                     " zeroinitializer, ptr " + slot);
                for (const InitEntry& entry : e->entries) {
                    for (const CImpl::FieldInfo& field : im->fields) {
                        if (field.name != entry.name) continue;
                        EV value = eval(entry.value.get(), field.ty);
                        line("store " + std::string(ll(field.ty)) + " " + value.first +
                             ", ptr " + slot);
                    }
                }
                std::string result = reg();
                line(result + " = load " + std::string(ll(result_ty)) + ", ptr " + slot);
                return {result, result_ty};
            }
            std::string aggregate = "zeroinitializer";
            for (size_t i = 0; i < im->fields.size(); i++) {
                const CImpl::FieldInfo& field = im->fields[i];
                const InitEntry* given = nullptr;
                for (const InitEntry& entry : e->entries) {
                    if (entry.name == field.name) given = &entry;
                }
                const Expr* source = given ? given->value.get() : field.decl->def.get();
                if (!source) continue;
                EV value = eval(source, field.ty);
                std::string next = reg();
                line(next + " = insertvalue " + std::string(ll(result_ty)) + " " +
                     aggregate + ", " + ll(field.ty) + " " + value.first + ", " +
                     std::to_string(i));
                aggregate = next;
            }
            return {aggregate, result_ty};
        }
        long long mask = 0;
        for (const CImpl::FieldInfo& f : im->fields) {
            if (is_rc(f.ty)) mask |= 1LL << (f.offset / 8);
        }
        std::string o = alloc_bytes(im->size, fixed_meta(mask));
        line("store ptr @td_" + im->mangled + ", ptr " + o);
        for (const CImpl::FieldInfo& f : im->fields) {
            const InitEntry* given = nullptr;
            for (const InitEntry& en : e->entries) {
                if (en.name == f.name) given = &en;
            }
            if (given) {
                EV v = eval(given->value.get(), f.ty);
                transfer_in(v);
                store_at(o, f.offset, v.first, f.ty);
            } else if (f.decl->def) {
                EV v = eval(f.decl->def.get(), f.ty);
                transfer_in(v);
                store_at(o, f.offset, v.first, f.ty);
            } else if (f.ty->k == Ty::f64_) {
                store_at(o, f.offset, fmt_double(0), f.ty);
            } else if (f.ty->k == Ty::i64_ || f.ty->k == Ty::i1_ ||
                       f.ty->k == Ty::dec_) {
                store_at(o, f.offset, "0", f.ty);
            } else {
                store_at(o, f.offset, "null", f.ty);
            }
        }
        emit_fin_flag(o, im);
        own(o, cg.t_obj(im->mangled));
        return {o, cg.t_obj(im->mangled)};
    }

    // ---- calls ----
    std::vector<EV> eval_args(const Expr* e, const std::vector<Param>& params,
                              const std::map<std::string, Ty*>& penv) {
        std::vector<EV> out;
        for (size_t i = 0; i < e->args.size(); i++) {
            Ty* h = i < params.size()
                        ? cg.resolve(params[i].type.get(), penv, e->line, e->col)
                        : nullptr;
            EV v = eval(e->args[i].get(), h);
            pin_borrow(e->args[i].get(), v);
            if (i < params.size() && params[i].passing == Param::Passing::take)
                transfer_in(v);
            out.push_back(v);
        }
        return out;
    }
    std::string args_text(const std::vector<EV>& args, const std::string& self_val,
                          const std::vector<Param>* params = nullptr) {
        std::string s;
        bool first = true;
        if (!self_val.empty()) {
            s += "ptr " + self_val;
            first = false;
        }
        for (size_t i = 0; i < args.size(); i++) {
            const EV& a = args[i];
            if (!first) s += ", ";
            first = false;
            if (params && i < params->size() &&
                (*params)[i].passing == Param::Passing::inout)
                s += "ptr " + a.first;
            else
                s += std::string(ll(a.second)) + " " + a.first;
        }
        return s;
    }

    EV emit_extern_bridge(const FnDecl* function, Ty* result,
                          const std::vector<EV>& args) {
        size_t slots = std::max<size_t>(args.size(), 1);
        std::string pointers = "%v" + std::to_string(next_reg++) + ".ffiargs";
        allocas += "  " + pointers + " = alloca [" + std::to_string(slots) +
                   " x ptr]\n";
        for (size_t i = 0; i < args.size(); i++) {
            std::string value = fresh_slot("ffiarg", args[i].second);
            line("store " + std::string(ll(args[i].second)) + " " + args[i].first +
                 ", ptr " + value);
            std::string place = reg();
            line(place + " = getelementptr [" + std::to_string(slots) +
                 " x ptr], ptr " + pointers + ", i64 0, i64 " + std::to_string(i));
            line("store ptr " + value + ", ptr " + place);
        }
        std::string result_slot = "null";
        if (result->k != Ty::unit_) result_slot = fresh_slot("ffiret", result);
        line("call void " + cg.request_extern(function) + "(ptr " + result_slot +
             ", ptr " + pointers + ")");
        if (result->k == Ty::unit_) return {"", result};
        std::string value = reg();
        line(value + " = load " + std::string(ll(result)) + ", ptr " + result_slot);
        return {value, result};
    }

    // plain or monomorphized call of a top-level fn, local or from a package
    EV call_top_fn(const Expr* e, const FnDecl* f) {
        if (f->generics.empty()) {
            std::vector<EV> args = eval_args(e, f->params, CG2::empty_env());
            Ty* ret = cg.resolve(f->ret.get(), CG2::empty_env(), e->line, e->col);
            if (f->is_extern_c && cg.extern_has_aggregate(f))
                return emit_extern_bridge(f, ret, args);
            std::string symbol = f->is_extern_c ? cg.request_extern(f)
                                                 : "@b_" + f->qualname;
            return emit_call(symbol, ret,
                             args_text(args, "", &f->params));
        }
        // monomorphize: infer the generic params from the argument types
        std::set<std::string> gens;
        for (const GenericParam& g : f->generics) gens.insert(g.name);
        std::vector<EV> args;
        std::map<std::string, Ty*> fenv;
        for (size_t i = 0; i < e->args.size(); i++) {
            EV a = eval(e->args[i].get());
            if (i < f->params.size()) {
                cg.unify_tref(f->params[i].type.get(), a.second, gens, fenv);
            }
            if (i < f->params.size() &&
                f->params[i].passing == Param::Passing::take)
                transfer_in(a);
            args.push_back(std::move(a));
        }
        std::string sym = cg.request_fn(f, fenv);
        Ty* ret = cg.resolve(f->ret.get(), fenv, e->line, e->col);
        return emit_call(sym, ret, args_text(args, "", &f->params));
    }
    EV emit_call(const std::string& target, Ty* ret, const std::string& args) {
        if (ret->k == Ty::unit_) {
            line("call void " + target + "(" + args + ")");
            return {"", ret};
        }
        std::string r = reg();
        line(r + " = call " + std::string(ll(ret)) + " " + target + "(" + args + ")");
        own(r, ret); // beans functions return +1
        return {r, ret};
    }

    EV eval_call(const Expr* e, Ty* hint) {
        const Expr* callee = e->callee.get();

        if (callee->kind == Expr::Kind::ident) {
            std::string name(callee->text);
            if (Var* v = find_var(name)) {
                EV fnv = {var_read(v), v->ty};
                if (fnv.second->k == Ty::fn_) {
                    if (!v->direct_fn.empty())
                        return call_direct_fn_value(fnv, e, v->direct_fn);
                    return call_fn_value(fnv, e);
                }
                err(e, "calling this");
                return {"0", cg.t_i64()};
            }
            if (name == "some") {
                Ty* inner = hint && hint->k == Ty::enum_ && !hint->args.empty()
                                ? hint->args[0]
                                : nullptr;
                EV v = eval(e->args[0].get(), inner);
                Ty* actual = inner ? inner : v.second;
                std::string b = make_option_some(v, actual);
                return {b, cg.t_option(actual)};
            }
            if (name == "ok") {
                Ty* inner = hint && hint->k == Ty::enum_ && !hint->args.empty()
                                ? hint->args[0]
                                : nullptr;
                EV v = eval(e->args[0].get(), inner);
                Ty* ok_type = inner ? inner : v.second;
                Ty* error_type = hint && hint->args.size() >= 2 ? hint->args[1]
                                                                : cg.t_error();
                std::string b = make_result_value(true, v, ok_type, error_type);
                return {b, cg.t_result(ok_type, error_type)};
            }
            if (name == "err") {
                EV v = eval(e->args[0].get());
                std::string payload = v.first;
                Ty* pty = v.second;
                if (v.second->k == Ty::str_) {
                    payload = make_error(v.first);
                    pty = cg.t_error();
                }
                Ty* ok = hint && hint->k == Ty::enum_ && !hint->args.empty()
                             ? hint->args[0]
                             : cg.t_i64();
                Ty* error_type = hint && hint->args.size() >= 2 ? hint->args[1] : pty;
                std::string b = make_result_value(false, {payload, pty}, ok, error_type);
                return {b, cg.t_result(ok, error_type)};
            }
            auto fit = cg.fn_decls.find(callee->resolved.empty() ? cg.qual(name)
                                                                 : callee->resolved);
            if (fit != cg.fn_decls.end()) {
                return call_top_fn(e, fit->second);
            }
            // a class name called like a function: construction through init.
            // Same resolution ladder as eval_init — checker key, plain name,
            // current package (re-parsed interpolation segments).
            {
                const ClassDecl* cd = nullptr;
                if (!callee->resolved.empty()) {
                    auto cit = cg.class_decls.find(callee->resolved);
                    if (cit != cg.class_decls.end()) cd = cit->second;
                } else {
                    auto cit = cg.class_decls.find(name);
                    if (cit == cg.class_decls.end())
                        cit = cg.class_decls.find(cg.qual(name));
                    if (cit != cg.class_decls.end()) cd = cit->second;
                }
                if (cd) return eval_ctor(e, cd, hint);
            }
            err(e, "calling '" + name + "'");
            return {"0", cg.t_i64()};
        }

        if (callee->kind != Expr::Kind::field) {
            EV fnv = eval(callee);
            if (fnv.second->k == Ty::fn_) return call_fn_value(fnv, e);
            err(e, "this call");
            return {"0", cg.t_i64()};
        }
        const Expr* obj = callee->object.get();
        const std::string& mname = callee->name;

        // module functions from the registry, args assembled the same way
        // whether the checker pinned the call or the fallback matched it
        auto emit_builtin_fn = [&](const BuiltinFn& b) -> EV {
            std::string args;
            for (size_t i = 0; i < b.params.size() && i < e->args.size(); i++) {
                Ty* pt = bty(b.params[i]);
                EV a = eval(e->args[i].get(), pt);
                if (i) args += ", ";
                args += barg(a, pt);
            }
            if (b.panics) {
                if (!args.empty()) args += ", ";
                args += "i64 " + std::to_string(e->line) + ", i64 " +
                        std::to_string(e->col);
            }
            return emit_bcall(b.sym, b.ret, args, nullptr, e);
        };
        auto emit_print = [&](const std::string& which) -> EV {
            EV v = eval(e->args[0].get());
            std::string s = to_str(v, e->args[0].get());
            line("call void @beans_" + which + "(ptr " + s + ")");
            return {"", cg.t_unit()};
        };

        // the checker pinned this call: a std builtin or a package function
        if (!callee->resolved.empty()) {
            const std::string& r = callee->resolved;
            if (r == "std.io.println") return emit_print("println");
            if (r == "std.io.print") return emit_print("print");
            if (r == "std.io.eprintln") return emit_print("eprintln");
            if (r == "std.io.eprint") return emit_print("eprint");
            for (const BuiltinFn& b : builtin_fns()) {
                if (r == std::string(b.module) + "." + b.name) return emit_builtin_fn(b);
            }
            auto rfit = cg.fn_decls.find(r);
            if (r != "std.thread.spawn" && rfit != cg.fn_decls.end()) {
                return call_top_fn(e, rfit->second);
            }
            // super.init(...): direct call of the resolved ancestor's init on
            // the live self — construction is not restarted
            if (mname == "init" && obj->kind == Expr::Kind::ident &&
                obj->text == "super" && self_impl) {
                CImpl* anc = nullptr;
                for (CImpl* p = self_impl->parent; p; p = p->parent) {
                    if (p->decl->qualname == r) { anc = p; break; }
                }
                const FnDecl* ini = nullptr;
                if (anc) {
                    for (const FnDecl& m : anc->decl->methods) {
                        if (m.has_self && m.name == "init") ini = &m;
                    }
                }
                Var* sv = find_var("self");
                if (!ini || !sv) {
                    err(e, "super.init here");
                    return {"", cg.t_unit()};
                }
                std::vector<EV> args = eval_args(e, ini->params, anc->env);
                emit_call("@m_" + anc->mangled + "_init", cg.t_unit(),
                          args_text(args, var_read(sv), &ini->params));
                return {"", cg.t_unit()};
            }
            // pkg.Class(args) — the checker pinned the class key
            auto rcit = cg.class_decls.find(r);
            if (rcit != cg.class_decls.end()) return eval_ctor(e, rcit->second, hint);
        }

        if (obj->kind == Expr::Kind::ident && !find_var(std::string(obj->text))) {
            std::string n(obj->text);
            std::string bpath = cg.binding_path(n);
            if (bpath == "std.io" &&
                (mname == "eprintln" || mname == "eprint")) {
                return emit_print(mname);
            }
            for (const BuiltinFn& b : builtin_fns()) {
                if (bpath == b.module && mname == b.name) return emit_builtin_fn(b);
            }
            if (bpath == "std.io" && (mname == "println" || mname == "print")) {
                EV v = eval(e->args[0].get());
                std::string s = to_str(v, e->args[0].get());
                line("call void @beans_" + mname + "(ptr " + s + ")");
                return {"", cg.t_unit()};
            }
            if (!bpath.empty() && bpath != "std.io" && bpath != "std.thread") {
                // unannotated package call (string-interpolation segments)
                auto pfx = cg.prefix_by_path.find(bpath);
                if (pfx != cg.prefix_by_path.end()) {
                    auto fit = cg.fn_decls.find(pfx->second + "." + mname);
                    if (fit != cg.fn_decls.end()) return call_top_fn(e, fit->second);
                }
            }
            // enum construction
            std::string en = obj->resolved.empty() ? cg.qual(n) : obj->resolved;
            if (cg.enum_decls.count(en)) {
                const EnumVariant* var = cg.variant_decl(en, mname);
                int tag = cg.variant_tag(en, mname);
                std::vector<EV> payload;
                for (size_t i = 0; i < e->args.size(); i++) {
                    Ty* h = var && i < var->payload.size()
                                ? cg.resolve(var->payload[i].type.get(), CG2::empty_env(),
                                             e->line, e->col)
                                : nullptr;
                    payload.push_back(eval(e->args[i].get(), h));
                }
                std::string b = box_enum(tag, payload);
                return {b, cg.t_enum(en, {})};
            }
            // user class static
            auto cit = cg.class_decls.find(obj->resolved.empty() ? cg.qual(n)
                                                                 : obj->resolved);
            if (cit != cg.class_decls.end() && !cit->second->is_interface) {
                CImpl* im = cg.request_impl(cit->second, {}, e->line, e->col);
                for (const FnDecl& m : cit->second->methods) {
                    if (m.name == mname && !m.has_self) {
                        std::vector<EV> args = eval_args(e, m.params, im->env);
                        Ty* ret = cg.resolve(m.ret.get(), im->env, e->line, e->col);
                        return emit_call("@s_" + im->mangled + "_" + mname, ret,
                                         args_text(args, "", &m.params));
                    }
                }
            }
            if ((bpath == "std.thread" ||
                 (!callee->resolved.empty() && callee->resolved == "std.thread.spawn")) &&
                mname == "spawn") {
                EV clo = eval(e->args[0].get());
                Ty* ret = clo.second->k == Ty::fn_ && clo.second->fn_ret()
                              ? clo.second->fn_ret()
                              : cg.t_unit();
                // per-site thunk: (ptr env) -> i64, so the C runtime can call it
                std::string thunk = "@spawn_thunk" + std::to_string(cg.next_clo++);
                std::string t;
                t += "define i64 " + thunk + "(ptr %env) {\n";
                t += "  %f = load ptr, ptr %env\n";
                if (ret->k == Ty::unit_) {
                    t += "  call void %f(ptr %env)\n  ret i64 0\n}\n\n";
                } else if (ret->k == Ty::f64_) {
                    t += "  %r = call " + std::string(ll(ret)) + " %f(ptr %env)\n";
                    if (ret->bits == 32) {
                        t += "  %b32 = bitcast float %r to i32\n";
                        t += "  %b = zext i32 %b32 to i64\n  ret i64 %b\n}\n\n";
                    } else {
                        t += "  %b = bitcast double %r to i64\n  ret i64 %b\n}\n\n";
                    }
                } else if (ret->k == Ty::i64_) {
                    t += "  %r = call " + std::string(ll(ret)) + " %f(ptr %env)\n";
                    if (ret->bits == 64) {
                        t += "  ret i64 %r\n}\n\n";
                    } else {
                        t += "  %z = " + std::string(ret->is_unsigned ? "zext" : "sext") +
                             " " + std::string(ll(ret)) + " %r to i64\n  ret i64 %z\n}\n\n";
                    }
                } else if (ret->k == Ty::i1_) {
                    t += "  %r = call i1 %f(ptr %env)\n";
                    t += "  %z = zext i1 %r to i64\n  ret i64 %z\n}\n\n";
                } else if (ret->k == Ty::dec_) {
                    t += "  %r = call i128 %f(ptr %env)\n";
                    t += "  %p = call ptr @beans_decv_box(i128 %r)\n";
                    t += "  %z = ptrtoint ptr %p to i64\n  ret i64 %z\n}\n\n";
                } else {
                    t += "  %r = call ptr %f(ptr %env)\n";
                    t += "  %z = ptrtoint ptr %r to i64\n  ret i64 %z\n}\n\n";
                }
                cg.lifted += t;
                transfer_in(clo);
                std::string r = reg();
                line(r + " = call ptr @beans_thread_spawn(ptr " + thunk + ", ptr " +
                     clo.first + ")");
                own(r, cg.t_kind1(Ty::thread_, ret));
                return {r, cg.t_kind1(Ty::thread_, ret)};
            }
            if (n == "Slice" && mname == "from_raw") {
                Ty* element = hint && hint->k == Ty::slice_ ? hint->args[0]
                                                             : cg.t_i64();
                EV pointer = eval(e->args[0].get(), cg.t_rawptr(element));
                EV length = eval(e->args[1].get(), cg.t_i64());
                std::string negative = reg();
                line(negative + " = icmp slt i64 " + length.first + ", 0");
                std::string neg_bad = bb(), after_negative = bb();
                line("br i1 " + negative + ", label %" + neg_bad + ", label %" +
                     after_negative);
                label(neg_bad);
                line("call void @beans_panic(ptr " +
                     cg.intern_string("negative slice length") + ", i64 " +
                     std::to_string(e->line) + ", i64 " +
                     std::to_string(e->col) + ")");
                line("unreachable");
                label(after_negative);
                std::string nonempty = reg(), nullp = reg(), invalid = reg();
                line(nonempty + " = icmp ne i64 " + length.first + ", 0");
                line(nullp + " = icmp eq ptr " + pointer.first + ", null");
                line(invalid + " = and i1 " + nonempty + ", " + nullp);
                std::string null_bad = bb(), ok = bb();
                line("br i1 " + invalid + ", label %" + null_bad + ", label %" + ok);
                label(null_bad);
                line("call void @beans_panic(ptr " +
                     cg.intern_string("null pointer with non-empty slice") +
                     ", i64 " + std::to_string(e->line) + ", i64 " +
                     std::to_string(e->col) + ")");
                line("unreachable");
                label(ok);
                std::string with_pointer = reg(), result = reg();
                line(with_pointer + " = insertvalue {ptr, i64} poison, ptr " +
                     pointer.first + ", 0");
                line(result + " = insertvalue {ptr, i64} " + with_pointer +
                     ", i64 " + length.first + ", 1");
                return {result, cg.t_slice(element)};
            }

            if (n == "Simd4f32") {
                Ty* vector = cg.t_simd4f32();
                Ty* f32 = cg.t_float(32);
                if (mname == "splat") {
                    EV value = eval(e->args[0].get(), f32);
                    std::string first = reg(), result = reg();
                    line(first + " = insertelement <4 x float> poison, float " +
                         value.first + ", i32 0");
                    line(result + " = shufflevector <4 x float> " + first +
                         ", <4 x float> poison, <4 x i32> zeroinitializer");
                    return {result, vector};
                }
                if (mname == "of") {
                    std::string current = "poison";
                    for (size_t i = 0; i < 4; i++) {
                        EV value = eval(e->args[i].get(), f32);
                        std::string next = reg();
                        line(next + " = insertelement <4 x float> " + current +
                             ", float " + value.first + ", i32 " +
                             std::to_string(i));
                        current = next;
                    }
                    return {current, vector};
                }
                if (mname == "load") {
                    EV pointer = eval(e->args[0].get(), cg.t_rawptr(f32));
                    std::string nullp = reg();
                    line(nullp + " = icmp eq ptr " + pointer.first + ", null");
                    std::string bad = bb(), ok = bb();
                    line("br i1 " + nullp + ", label %" + bad + ", label %" + ok);
                    label(bad);
                    line("call void @beans_panic(ptr " +
                         cg.intern_string("null SIMD load") + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                    std::string result = reg();
                    line(result + " = load <4 x float>, ptr " + pointer.first +
                         ", align 1");
                    return {result, vector};
                }
            }
            if (n == "RawPtr") {
                Ty* inner = hint && hint->k == Ty::rawptr_ ? hint->args[0] : cg.t_i64();
                Ty* result = cg.t_rawptr(inner);
                if (mname == "alloc") {
                    EV count = eval(e->args[0].get(), cg.t_i64());
                    std::string r = reg();
                    line(r + " = call ptr @beans_raw_alloc(i64 " + count.first +
                         ", i64 " + std::to_string(CG2::value_size(inner)) +
                         ", i64 " + std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    return {r, result};
                }
                if (mname == "from_address") {
                    EV address = eval(e->args[0].get(), cg.t_int(64, true));
                    std::string r = reg();
                    line(r + " = inttoptr i64 " + address.first + " to ptr");
                    return {r, result};
                }
                if (mname == "null") return {"null", result};
            }
            if (n == "Arena" && mname == "new") {
                EV cap = eval(e->args[0].get(), cg.t_i64());
                Ty* inner = hint && hint->k == Ty::arena_ ? hint->args[0] : cg.t_i64();
                std::string r = reg();
                line(r + " = call ptr @beans_arena_new(i64 " + cap.first + ", i64 " +
                     std::string(is_slot_rc(inner) ? "1" : "0") + ", i64 " +
                     std::to_string(e->line) + ", i64 " + std::to_string(e->col) + ")");
                Ty* result = cg.t_arena(inner);
                own(r, result);
                return {r, result};
            }
            if (n == "Box" && mname == "new") {
                Ty* inner = hint && hint->k == Ty::box_ ? hint->args[0] : nullptr;
                EV v = eval(e->args[0].get(), inner);
                transfer_in(v);
                std::string r = reg();
                line(r + " = call ptr @beans_box_new(i64 " + to_slot(v, true) +
                     ", i64 " + std::string(is_slot_rc(v.second) ? "1" : "0") +
                     ")");
                Ty* result = cg.t_box(inner ? inner : v.second);
                own(r, result);
                return {r, result};
            }
            if (n == "Shared" && mname == "new") {
                Ty* inner = hint && hint->k == Ty::shared_ ? hint->args[0] : nullptr;
                EV v = eval(e->args[0].get(), inner);
                transfer_in(v);
                std::string r = reg();
                line(r + " = call ptr @beans_shared_new(i64 " + to_slot(v, true) +
                     ", i64 " + std::string(is_slot_rc(v.second) ? "1" : "0") +
                     ")");
                Ty* result = cg.t_shared(inner ? inner : v.second);
                own(r, result);
                return {r, result};
            }
            if (n == "Mutex" && mname == "new") {
                Ty* inner = hint && hint->k == Ty::mutex_ ? hint->args[0] : nullptr;
                EV v = eval(e->args[0].get(), inner);
                transfer_in(v);
                std::string r = reg();
                line(r + " = call ptr @beans_mutex_new(i64 " + to_slot(v, true) +
                     ", i64 " + std::string(is_slot_rc(v.second) ? "1" : "0") +
                     ")");
                own(r, cg.t_kind1(Ty::mutex_, v.second));
                return {r, cg.t_kind1(Ty::mutex_, inner ? inner : v.second)};
            }
            if (n == "Channel" && mname == "new") {
                EV cap = eval(e->args[0].get());
                Ty* elem = hint && hint->k == Ty::chan_ ? hint->args[0] : cg.t_i64();
                std::string r = reg();
                line(r + " = call ptr @beans_chan_new(i64 " + cap.first + ", i64 " +
                     std::string(is_slot_rc(elem) ? "1" : "0") + ")");
                own(r, cg.t_kind1(Ty::chan_, elem));
                return {r, cg.t_kind1(Ty::chan_, elem)};
            }
            if (n == "AtomicInt" && mname == "new") {
                EV v = eval(e->args[0].get());
                std::string r = reg();
                line(r + " = call ptr @beans_atomic_new(i64 " + v.first + ")");
                own(r, cg.t_atomic());
                return {r, cg.t_atomic()};
            }
            if (n == "Bytes" || n == "File" || n == "Dir" || n == "MMap") {
                for (const BuiltinStatic& b : builtin_statics()) {
                    if (n != b.cls || mname != b.name) continue;
                    std::string args;
                    for (size_t i = 0; i < b.params.size() && i < e->args.size(); i++) {
                        Ty* pt = bty(b.params[i]);
                        EV a = eval(e->args[i].get(), pt);
                        if (i) args += ", ";
                        args += barg(a, pt);
                    }
                    if (b.panics) {
                        if (!args.empty()) args += ", ";
                        args += "i64 " + std::to_string(e->line) + ", i64 " +
                                std::to_string(e->col);
                    }
                    return emit_bcall(b.sym, b.ret, args, nullptr, e);
                }
            }
        }

        // `money.Payment.card(...)` / `money.Money.new(...)` — the receiver is
        // a field expression the checker resolved to a type
        if (obj->kind == Expr::Kind::field && !obj->resolved.empty()) {
            const std::string& tn = obj->resolved;
            if (cg.enum_decls.count(tn)) {
                const EnumVariant* var = cg.variant_decl(tn, mname);
                int tag = cg.variant_tag(tn, mname);
                std::vector<EV> payload;
                for (size_t i = 0; i < e->args.size(); i++) {
                    Ty* h = var && i < var->payload.size()
                                ? cg.resolve(var->payload[i].type.get(), CG2::empty_env(),
                                             e->line, e->col)
                                : nullptr;
                    payload.push_back(eval(e->args[i].get(), h));
                }
                std::string b = box_enum(tag, payload);
                return {b, cg.t_enum(tn, {})};
            }
            auto cit = cg.class_decls.find(tn);
            if (cit != cg.class_decls.end() && !cit->second->is_interface) {
                CImpl* im = cg.request_impl(cit->second, {}, e->line, e->col);
                for (const FnDecl& m : cit->second->methods) {
                    if (m.name == mname && !m.has_self) {
                        std::vector<EV> args = eval_args(e, m.params, im->env);
                        Ty* ret = cg.resolve(m.ret.get(), im->env, e->line, e->col);
                        return emit_call("@s_" + im->mangled + "_" + mname, ret,
                                         args_text(args, "", &m.params));
                    }
                }
            }
        }

        // Scalarize the very common `list.pop().or(default)` chain. A scalar
        // Option is still boxed at ABI/storage boundaries, but this temporary
        // never escapes: keep its has-bit in control flow and move the popped
        // slot straight to the result. Besides avoiding allocation, this keeps
        // source order: pop happens before the eagerly evaluated default.
        if (mname == "or" && e->args.size() == 1 && obj->kind == Expr::Kind::call &&
            obj->args.empty() && obj->callee &&
            obj->callee->kind == Expr::Kind::field && obj->callee->name == "pop") {
            const Expr* list_expr = obj->callee->object.get();
            EV list = eval(list_expr);
            if (list.second->k == Ty::list_) {
                Ty* elem = list.second->args[0];
                std::string len = load_at(list.first, 8, cg.t_i64());
                std::string has = reg();
                line(has + " = icmp sgt i64 " + len + ", 0");
                std::string someb = bb(), noneb = bb(), popped_end = bb();
                std::string popped_slot = fresh_slot("popraw", elem);
                line("br i1 " + has + ", label %" + someb + ", label %" + noneb);
                label(someb);
                std::string n1 = reg();
                line(n1 + " = sub i64 " + len + ", 1");
                store_at(list.first, 8, n1, cg.t_i64());
                std::string data = load_at(list.first, 0, cg.t_str());
                std::string ep = reg(), raw = reg();
                line(ep + " = getelementptr i64, ptr " + data + ", i64 " + n1);
                line(raw + " = load i64, ptr " + ep);
                std::string popped = from_slot(raw, elem, true);
                line("store " + std::string(ll(elem)) + " " + popped + ", ptr " +
                     popped_slot);
                line("br label %" + popped_end);
                label(noneb);
                line("br label %" + popped_end);
                label(popped_end);

                EV dflt = eval(e->args[0].get(), elem);
                if (is_rc(elem)) transfer_in(dflt);
                std::string use_popped = bb(), use_default = bb(), end = bb();
                std::string out = fresh_slot("popor", elem);
                line("br i1 " + has + ", label %" + use_popped + ", label %" +
                     use_default);
                label(use_popped);
                std::string value = reg();
                line(value + " = load " + std::string(ll(elem)) + ", ptr " +
                     popped_slot);
                if (is_rc(elem)) emit_release(dflt.first);
                line("store " + std::string(ll(elem)) + " " + value + ", ptr " + out);
                line("br label %" + end);
                label(use_default);
                line("store " + std::string(ll(elem)) + " " + dflt.first + ", ptr " +
                     out);
                line("br label %" + end);
                label(end);
                std::string result = reg();
                line(result + " = load " + std::string(ll(elem)) + ", ptr " + out);
                own(result, elem);
                return {result, elem};
            }
        }

        EV recv = eval(obj);
        pin_borrow(obj, recv);
        return method_call(e, recv, mname);
    }

    std::string make_error(const std::string& msg_val) {
        std::string o = alloc_bytes(32, fixed_meta((1LL << 2) | (1LL << 3)));
        line("store ptr null, ptr " + o);
        store_at(o, 8, "-1", cg.t_i64());
        transfer_in({msg_val, cg.t_str()});
        store_at(o, 16, msg_val, cg.t_str());
        store_at(o, 24, cg.intern_string(""), cg.t_str());
        own(o, cg.t_error());
        return o;
    }

    // table-driven builtin method (builtins.cpp): eval args per signature,
    // call the C symbol, box fallible returns from the BRes {val, msg} ABI —
    // 16 bytes so both C and IR return it in registers; msg null = ok.
    // Rows with `panics` pass (line, col) so the runtime's message carries
    // the source position, exactly like the interpreter's.
    Ty* bty(BT t) {
        switch (t) {
            case BT::f64: return cg.t_f64();
            case BT::dec: return cg.t_dec();
            case BT::boolean: return cg.t_bool();
            case BT::str: return cg.t_str();
            case BT::bytes: return cg.t_bytes();
            case BT::file: return cg.t_file();
            case BT::mmap: return cg.t_mmap();
            case BT::list_str: return cg.t_list(cg.t_str());
            default: return cg.t_i64();
        }
    }
    // registry args cross into C as i64/double/ptr; bools widen to i64
    std::string barg(const EV& a, Ty* pt) {
        if (pt->k == Ty::i1_) {
            std::string z = reg();
            line(z + " = zext i1 " + a.first + " to i64");
            return "i64 " + z;
        }
        return std::string(ll(pt)) + " " + a.first;
    }
    // the shared tail of every registry call: emit the C call for `sym` with
    // `args` already assembled, box the return per its BT shape. `recv` is
    // null for statics and module functions.
    EV emit_bcall(const char* sym, BT ret, const std::string& args, const EV* recv,
                  const Expr* e) {
        (void)e;
        if (ret == BT::self_recv) {
            line("call void @" + std::string(sym) + "(" + args + ")");
            return {recv->first, recv->second}; // the receiver, still borrowed
        }
        if (ret == BT::opt_i64 || ret == BT::opt_str) {
            Ty* ok_t = ret == BT::opt_str ? cg.t_str() : cg.t_i64();
            std::string sr = reg();
            line(sr + " = call {i64, i64} @" + std::string(sym) + "(" + args + ")");
            std::string raw = reg(), has = reg(), c = reg();
            line(raw + " = extractvalue {i64, i64} " + sr + ", 0");
            line(has + " = extractvalue {i64, i64} " + sr + ", 1");
            line(c + " = icmp ne i64 " + has + ", 0");
            std::string someb = bb(), noneb = bb(), endb = bb();
            std::string slot = fresh_slot("bop", cg.t_str());
            line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
            label(someb);
            size_t smark = temps.size();
            std::string okv = from_slot(raw, ok_t);
            if (is_rc(ok_t)) own(okv, ok_t); // runtime hands over its ref
            std::string sb = make_option_some({okv, ok_t}, ok_t);
            consume(sb);
            line("store ptr " + sb + ", ptr " + slot);
            flush_temps(smark);
            line("br label %" + endb);
            label(noneb);
            std::string nb = make_option_none(ok_t);
            consume(nb);
            line("store ptr " + nb + ", ptr " + slot);
            line("br label %" + endb);
            label(endb);
            std::string r = reg();
            line(r + " = load ptr, ptr " + slot);
            own(r, cg.t_option(ok_t));
            return {r, cg.t_option(ok_t)};
        }
        switch (ret) {
            case BT::unit:
                line("call void @" + std::string(sym) + "(" + args + ")");
                return {"", cg.t_unit()};
            case BT::boolean: {
                std::string c = reg(), r = reg();
                line(c + " = call i64 @" + std::string(sym) + "(" + args + ")");
                line(r + " = icmp ne i64 " + c + ", 0");
                return {r, cg.t_bool()};
            }
            case BT::res_i64:
            case BT::res_f64:
            case BT::res_dec:
            case BT::res_str:
            case BT::res_bool:
            case BT::res_bytes:
            case BT::res_file:
            case BT::res_mmap:
            case BT::res_list_str: {
                Ty* ok_t = ret == BT::res_f64        ? cg.t_f64()
                           : ret == BT::res_dec      ? cg.t_dec()
                           : ret == BT::res_str      ? cg.t_str()
                           : ret == BT::res_bool     ? cg.t_bool()
                           : ret == BT::res_bytes    ? cg.t_bytes()
                           : ret == BT::res_file     ? cg.t_file()
                           : ret == BT::res_mmap     ? cg.t_mmap()
                           : ret == BT::res_list_str ? cg.t_list(cg.t_str())
                                                     : cg.t_i64();
                std::string sr = reg();
                line(sr + " = call {i64, ptr} @" + std::string(sym) + "(" + args + ")");
                std::string raw = reg(), errp = reg(), c = reg();
                line(raw + " = extractvalue {i64, ptr} " + sr + ", 0");
                line(errp + " = extractvalue {i64, ptr} " + sr + ", 1");
                line(c + " = icmp eq ptr " + errp + ", null");
                std::string okb = bb(), errb = bb(), endb = bb();
                std::string slot = fresh_slot("res", cg.t_str());
                line("br i1 " + c + ", label %" + okb + ", label %" + errb);
                label(okb);
                size_t okmark = temps.size();
                std::string okv;
                if (ok_t->k == Ty::i1_) {
                    okv = reg();
                    line(okv + " = icmp ne i64 " + raw + ", 0");
                } else {
                    okv = from_slot(raw, ok_t, ok_t->k == Ty::dec_);
                    if (is_rc(ok_t)) own(okv, ok_t); // runtime hands over its ref
                }
                std::string ob = box_enum(0, {{okv, ok_t}});
                consume(ob);
                line("store ptr " + ob + ", ptr " + slot);
                flush_temps(okmark);
                line("br label %" + endb);
                label(errb);
                size_t ebmark = temps.size();
                // err is a ready-made Error object (msg + kind), rc 1, ours
                own(errp, cg.t_error());
                std::string eb = box_enum(1, {{errp, cg.t_error()}});
                consume(eb);
                line("store ptr " + eb + ", ptr " + slot);
                flush_temps(ebmark);
                line("br label %" + endb);
                label(endb);
                std::string r = reg();
                line(r + " = load ptr, ptr " + slot);
                own(r, cg.t_result(ok_t, cg.t_error()));
                return {r, cg.t_result(ok_t, cg.t_error())};
            }
            default: {
                Ty* rt2 = bty(ret);
                std::string r = reg();
                line(r + " = call " + std::string(ll(rt2)) + " @" + sym + "(" + args +
                     ")");
                if (is_rc(rt2)) own(r, rt2);
                return {r, rt2};
            }
        }
    }
    EV emit_builtin(const BuiltinMethod& b, const EV& recv, const Expr* e) {
        std::string args = "ptr " + recv.first;
        for (size_t i = 0; i < b.params.size() && i < e->args.size(); i++) {
            Ty* pt = bty(b.params[i]);
            EV a = eval(e->args[i].get(), pt);
            args += ", " + barg(a, pt);
        }
        if (b.panics) {
            args += ", i64 " + std::to_string(e->line) + ", i64 " +
                    std::to_string(e->col);
        }
        return emit_bcall(b.sym, b.ret, args, &recv, e);
    }

    EV method_call(const Expr* e, EV recv, const std::string& mname) {
        Ty* rt_ = recv.second;

        // user classes and interfaces: virtual dispatch
        if (rt_->k == Ty::obj_ && rt_->name != "Error") {
            CG2::FoundMethod fm;
            auto it = cg.impl_by_name.find(rt_->name);
            if (it != cg.impl_by_name.end()) {
                fm = cg.find_method_class(it->second, mname, false);
            } else if (rt_->iface) {
                fm = cg.find_method_iface(rt_->iface, mname);
            }
            if (!fm.decl) {
                // maybe a closure stored in a field
                if (it != cg.impl_by_name.end()) {
                    for (const CImpl::FieldInfo& f : it->second->fields) {
                        if (f.name == mname && f.ty->k == Ty::fn_) {
                            EV fnv = {load_at(recv.first, f.offset, f.ty), f.ty};
                            return call_fn_value(fnv, e);
                        }
                    }
                }
                err(e, "method '" + mname + "'");
                return {"0", cg.t_i64()};
            }
            std::vector<EV> args = eval_args(e, fm.decl->params, *fm.env);
            Ty* ret = cg.resolve(fm.decl->ret.get(), *fm.env, e->line, e->col);
            int slot = cg.selector(mname);
            std::string descriptor = load_at(recv.first, 0, cg.t_str());
            std::string sp = reg(), fp = reg();
            // Descriptor word 0 is the class id; method slots follow inline.
            // Keeping the table here saves one dependent load on every call.
            line(sp + " = getelementptr ptr, ptr " + descriptor + ", i64 " +
                 std::to_string(slot + 1));
            line(fp + " = load ptr, ptr " + sp);
            return emit_call(fp, ret,
                             args_text(args, recv.first, &fm.decl->params));
        }

        // enum methods (direct) and Option/Result builtins
        if (rt_->k == Ty::enum_) {
            auto eit = cg.enum_decls.find(rt_->name);
            if (eit != cg.enum_decls.end()) {
                for (const FnDecl& m : eit->second->methods) {
                    if (m.name == mname) {
                        std::vector<EV> args = eval_args(e, m.params, CG2::empty_env());
                        Ty* ret = cg.resolve(m.ret.get(), CG2::empty_env(), e->line, e->col);
                        return emit_call("@em_" + rt_->name + "_" + mname, ret,
                                         args_text(args, recv.first, &m.params));
                    }
                }
            }
            Ty* inner = rt_->args.empty() ? cg.t_i64() : rt_->args[0];
            if (mname == "or") {
                EV dflt = eval(e->args[0].get(), inner);
                if (has_owned_refs(inner)) transfer_in(dflt);
                std::string c = option_has(recv);
                std::string hasb = bb(), endb = bb();
                std::string slot = fresh_slot("orv", inner);
                line("store " + std::string(ll(inner)) + " " + dflt.first + ", ptr " + slot);
                line("br i1 " + c + ", label %" + hasb + ", label %" + endb);
                label(hasb);
                if (has_owned_refs(inner)) emit_release_value(dflt.first, inner);
                std::string payload = option_payload(recv, inner);
                if (has_owned_refs(inner)) emit_retain_value(payload, inner);
                line("store " + std::string(ll(inner)) + " " + payload + ", ptr " + slot);
                line("br label %" + endb);
                label(endb);
                std::string r = reg();
                line(r + " = load " + std::string(ll(inner)) + ", ptr " + slot);
                own(r, inner);
                return {r, inner};
            }
            if (mname == "expect") {
                EV msg = eval(e->args[0].get());
                std::string c;
                if (rt_->name == "Option") {
                    c = option_has(recv);
                } else {
                    c = result_is_ok(recv);
                }
                std::string okb = bb(), badb = bb();
                line("br i1 " + c + ", label %" + okb + ", label %" + badb);
                label(badb);
                line("call void @beans_panic(ptr " + msg.first + ", i64 " +
                     std::to_string(e->line) + ", i64 " + std::to_string(e->col) + ")");
                line("unreachable");
                label(okb);
                std::string payload = rt_->name == "Option"
                                          ? option_payload(recv, inner)
                                          : result_payload(recv, true);
                if (has_owned_refs(inner)) {
                    emit_retain_value(payload, inner);
                    own(payload, inner);
                }
                return {payload, inner};
            }
            if (mname == "is_some" || mname == "is_ok" || mname == "is_none") {
                std::string r;
                if (rt_->name == "Option") {
                    r = option_has(recv);
                    if (mname == "is_none") {
                        std::string inverted = reg();
                        line(inverted + " = xor i1 " + r + ", true");
                        r = inverted;
                    }
                } else {
                    r = result_is_ok(recv);
                }
                return {r, cg.t_bool()};
            }
            err(e, "method '" + mname + "'");
            return {"0", cg.t_i64()};
        }

        // builtins on primitives / string / list / decimal
        switch (rt_->k) {
            case Ty::i64_:
                if (mname == "abs") {
                    if (recv.second->is_unsigned) return recv;
                    std::string neg = reg(), c = reg(), r = reg();
                    std::string type = ll(recv.second);
                    line(neg + " = sub " + type + " 0, " + recv.first);
                    line(c + " = icmp slt " + type + " " + recv.first + ", 0");
                    line(r + " = select i1 " + c + ", " + type + " " + neg + ", " +
                         type + " " + recv.first);
                    r = normalize_integer(r, recv.second);
                    return {r, recv.second};
                }
                break;
            case Ty::f64_:
                if (mname == "abs") {
                    std::string r = reg();
                    std::string suffix = recv.second->bits == 32 ? "f32" : "f64";
                    line(r + " = call " + std::string(ll(recv.second)) + " @llvm.fabs." +
                         suffix + "(" + std::string(ll(recv.second)) + " " + recv.first + ")");
                    r = normalize_float(r, recv.second);
                    return {r, recv.second};
                }
                if (mname == "round") {
                    std::string r = reg();
                    line(r + " = call i64 @beans_f64_round(double " + as_f64(recv) + ")");
                    return {r, cg.t_i64()};
                }
                break;
            case Ty::dec_:
                if (mname == "abs") {
                    std::string r = reg();
                    line(r + " = call i128 @beans_decv_abs(i128 " + recv.first + ")");
                    return {r, cg.t_dec()};
                }
                if (mname == "round") {
                    EV p = eval(e->args[0].get());
                    std::string r = reg();
                    line(r + " = call i128 @beans_decv_round(i128 " + recv.first +
                         ", i64 " +
                         p.first + ")");
                    return {r, cg.t_dec()};
                }
                break;
            case Ty::str_: {
                for (const BuiltinMethod& b : builtin_methods()) {
                    if (b.recv == BT::str && mname == b.name) {
                        return emit_builtin(b, recv, e);
                    }
                }
                break;
            }
            case Ty::bytes_: {
                for (const BuiltinMethod& b : builtin_methods()) {
                    if (b.recv == BT::bytes && mname == b.name) {
                        return emit_builtin(b, recv, e);
                    }
                }
                break;
            }
            case Ty::file_: {
                for (const BuiltinMethod& b : builtin_methods()) {
                    if (b.recv == BT::file && mname == b.name) {
                        return emit_builtin(b, recv, e);
                    }
                }
                break;
            }
            case Ty::mmap_: {
                for (const BuiltinMethod& b : builtin_methods()) {
                    if (b.recv == BT::mmap && mname == b.name) {
                        return emit_builtin(b, recv, e);
                    }
                }
                break;
            }
            case Ty::fixed_array_:
                if (mname == "len")
                    return {std::to_string(rt_->array_len), cg.t_i64()};
                break;
            case Ty::simd4f32_: {
                if (mname == "lane") {
                    EV index = eval(e->args[0].get(), cg.t_i64());
                    std::string below = reg(), above = reg(), outside = reg();
                    line(below + " = icmp slt i64 " + index.first + ", 0");
                    line(above + " = icmp sgt i64 " + index.first + ", 3");
                    line(outside + " = or i1 " + below + ", " + above);
                    std::string bad = bb(), ok = bb();
                    line("br i1 " + outside + ", label %" + bad + ", label %" + ok);
                    label(bad);
                    line("call void @beans_panic(ptr " +
                         cg.intern_string("SIMD lane out of range") + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                    std::string result = reg();
                    line(result + " = extractelement <4 x float> " + recv.first +
                         ", i64 " + index.first);
                    return {result, cg.t_float(32)};
                }
                if (mname == "sum") {
                    std::string lane0 = reg(), lane1 = reg(), lane2 = reg(), lane3 = reg();
                    std::string pair0 = reg(), pair1 = reg(), result = reg();
                    line(lane0 + " = extractelement <4 x float> " + recv.first + ", i32 0");
                    line(lane1 + " = extractelement <4 x float> " + recv.first + ", i32 1");
                    line(lane2 + " = extractelement <4 x float> " + recv.first + ", i32 2");
                    line(lane3 + " = extractelement <4 x float> " + recv.first + ", i32 3");
                    line(pair0 + " = fadd float " + lane0 + ", " + lane1);
                    line(pair1 + " = fadd float " + lane2 + ", " + lane3);
                    line(result + " = fadd float " + pair0 + ", " + pair1);
                    return {result, cg.t_float(32)};
                }
                if (mname == "store") {
                    EV pointer = eval(e->args[0].get(), cg.t_rawptr(cg.t_float(32)));
                    std::string nullp = reg();
                    line(nullp + " = icmp eq ptr " + pointer.first + ", null");
                    std::string bad = bb(), ok = bb();
                    line("br i1 " + nullp + ", label %" + bad + ", label %" + ok);
                    label(bad);
                    line("call void @beans_panic(ptr " +
                         cg.intern_string("null SIMD store") + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                    line("store <4 x float> " + recv.first + ", ptr " + pointer.first +
                         ", align 1");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::slice_: {
                Ty* element = rt_->args[0];
                std::string pointer = reg(), length = reg();
                line(pointer + " = extractvalue {ptr, i64} " + recv.first + ", 0");
                line(length + " = extractvalue {ptr, i64} " + recv.first + ", 1");
                if (mname == "len") return {length, cg.t_i64()};
                if (mname == "as_ptr") return {pointer, cg.t_rawptr(element)};
                if (mname == "get" || mname == "set") {
                    EV index = eval(e->args[0].get(), cg.t_i64());
                    std::string inside = reg();
                    line(inside + " = icmp ult i64 " + index.first + ", " + length);
                    std::string ok = bb(), bad = bb();
                    line("br i1 " + inside + ", label %" + ok + ", label %" + bad);
                    label(bad);
                    line("call void @beans_panic_slice_index(i64 " + index.first +
                         ", i64 " + length + ", i64 " + std::to_string(e->line) +
                         ", i64 " + std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                    std::string item_pointer = reg();
                    line(item_pointer + " = getelementptr " + std::string(ll(element)) +
                         ", ptr " + pointer + ", i64 " + index.first);
                    if (mname == "get") {
                        std::string result = reg();
                        line(result + " = load " + std::string(ll(element)) +
                             ", ptr " + item_pointer + ", align 1");
                        return {result, element};
                    }
                    EV value = eval(e->args[1].get(), element);
                    line("store " + std::string(ll(element)) + " " + value.first +
                         ", ptr " + item_pointer + ", align 1");
                    return {"", cg.t_unit()};
                }
                if (mname == "subslice") {
                    EV from = eval(e->args[0].get(), cg.t_i64());
                    EV to = eval(e->args[1].get(), cg.t_i64());
                    std::string ordered = reg(), within = reg(), valid = reg();
                    line(ordered + " = icmp ule i64 " + from.first + ", " + to.first);
                    line(within + " = icmp ule i64 " + to.first + ", " + length);
                    line(valid + " = and i1 " + ordered + ", " + within);
                    std::string ok = bb(), bad = bb();
                    line("br i1 " + valid + ", label %" + ok + ", label %" + bad);
                    label(bad);
                    line("call void @beans_panic(ptr " +
                         cg.intern_string("slice range out of bounds") + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                    std::string start = reg(), count = reg();
                    line(start + " = getelementptr " + std::string(ll(element)) +
                         ", ptr " + pointer + ", i64 " + from.first);
                    line(count + " = sub i64 " + to.first + ", " + from.first);
                    std::string with_pointer = reg(), result = reg();
                    line(with_pointer + " = insertvalue {ptr, i64} poison, ptr " +
                         start + ", 0");
                    line(result + " = insertvalue {ptr, i64} " + with_pointer +
                         ", i64 " + count + ", 1");
                    return {result, rt_};
                }
                break;
            }
            case Ty::rawptr_: {
                Ty* inner = rt_->args[0];
                auto guard_null = [&](const char* message) {
                    std::string nullp = reg();
                    line(nullp + " = icmp eq ptr " + recv.first + ", null");
                    std::string bad = bb(), ok = bb();
                    line("br i1 " + nullp + ", label %" + bad + ", label %" + ok);
                    label(bad);
                    line("call void @beans_panic(ptr " + cg.intern_string(message) +
                         ", i64 " + std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                };
                auto guard_atomic_alignment = [&] {
                    int align = CG2::value_align(inner);
                    if (align <= 1) return;
                    std::string address = reg(), low = reg(), bad_value = reg();
                    line(address + " = ptrtoint ptr " + recv.first + " to i64");
                    line(low + " = and i64 " + address + ", " +
                         std::to_string(align - 1));
                    line(bad_value + " = icmp ne i64 " + low + ", 0");
                    std::string bad = bb(), ok = bb();
                    line("br i1 " + bad_value + ", label %" + bad + ", label %" + ok);
                    label(bad);
                    line("call void @beans_panic(ptr " +
                         cg.intern_string("unaligned raw pointer atomic access") +
                         ", i64 " + std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    line("unreachable");
                    label(ok);
                };
                if (mname == "read" || mname == "read_volatile") {
                    guard_null("null raw pointer read");
                    std::string value = reg();
                    line(value + " = load " +
                         std::string(mname == "read_volatile" ? "volatile " : "") +
                         std::string(ll(inner)) + ", ptr " + recv.first);
                    return {value, inner};
                }
                if (mname == "write" || mname == "write_volatile") {
                    EV value = eval(e->args[0].get(), inner);
                    guard_null("null raw pointer write");
                    line("store " +
                         std::string(mname == "write_volatile" ? "volatile " : "") +
                         std::string(ll(inner)) + " " + value.first + ", ptr " +
                         recv.first);
                    return {"", cg.t_unit()};
                }
                if (mname == "offset") {
                    EV count = eval(e->args[0].get(), cg.t_i64());
                    std::string result = reg();
                    line(result + " = getelementptr " + std::string(ll(inner)) +
                         ", ptr " + recv.first + ", i64 " + count.first);
                    return {result, rt_};
                }
                if (mname == "address") {
                    std::string address = reg();
                    line(address + " = ptrtoint ptr " + recv.first + " to i64");
                    return {address, cg.t_int(64, true)};
                }
                if (mname == "is_null") {
                    std::string result = reg();
                    line(result + " = icmp eq ptr " + recv.first + ", null");
                    return {result, cg.t_bool()};
                }
                if (mname == "element_size") {
                    return {std::to_string(CG2::value_size(inner)), cg.t_i64()};
                }
                if (mname == "element_align") {
                    return {std::to_string(CG2::value_align(inner)), cg.t_i64()};
                }
                if (mname == "copy_from") {
                    EV source = eval(e->args[0].get(), rt_);
                    EV count = eval(e->args[1].get(), cg.t_i64());
                    line("call void @beans_raw_copy(ptr " + recv.first + ", ptr " +
                         source.first + ", i64 " + count.first + ", i64 " +
                         std::to_string(CG2::value_size(inner)) + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "fill_zero") {
                    EV count = eval(e->args[0].get(), cg.t_i64());
                    line("call void @beans_raw_zero(ptr " + recv.first + ", i64 " +
                         count.first + ", i64 " +
                         std::to_string(CG2::value_size(inner)) + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "atomic_load") {
                    guard_null("null raw pointer atomic load");
                    guard_atomic_alignment();
                    std::string value = reg();
                    line(value + " = load atomic " + std::string(ll(inner)) +
                         ", ptr " + recv.first + " seq_cst, align " +
                         std::to_string(CG2::value_align(inner)));
                    return {value, inner};
                }
                if (mname == "atomic_store") {
                    EV value = eval(e->args[0].get(), inner);
                    guard_null("null raw pointer atomic store");
                    guard_atomic_alignment();
                    line("store atomic " + std::string(ll(inner)) + " " + value.first +
                         ", ptr " + recv.first + " seq_cst, align " +
                         std::to_string(CG2::value_align(inner)));
                    return {"", cg.t_unit()};
                }
                if (mname == "atomic_fetch_add") {
                    EV value = eval(e->args[0].get(), inner);
                    guard_null("null raw pointer atomic fetch_add");
                    guard_atomic_alignment();
                    std::string old = reg();
                    line(old + " = atomicrmw add ptr " + recv.first + ", " +
                         std::string(ll(inner)) + " " + value.first + " seq_cst");
                    return {old, inner};
                }
                if (mname == "atomic_compare_exchange") {
                    EV expected = eval(e->args[0].get(), inner);
                    EV desired = eval(e->args[1].get(), inner);
                    guard_null("null raw pointer atomic compare_exchange");
                    guard_atomic_alignment();
                    std::string pair = reg(), success = reg();
                    line(pair + " = cmpxchg ptr " + recv.first + ", " +
                         std::string(ll(inner)) + " " + expected.first + ", " +
                         std::string(ll(inner)) + " " + desired.first +
                         " seq_cst seq_cst");
                    line(success + " = extractvalue {" + std::string(ll(inner)) +
                         ", i1} " + pair + ", 1");
                    return {success, cg.t_bool()};
                }
                if (mname == "free") {
                    line("call void @beans_raw_free(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::list_: {
                Ty* elem = rt_->args[0];
                if (mname == "clone") {
                    std::string copy = reg();
                    line(copy + " = call ptr @beans_list_clone(ptr " + recv.first + ")");
                    Ty* result = cg.t_list(elem);
                    own(copy, result);
                    return {copy, result};
                }
                if (mname == "reserve") {
                    EV capacity = eval(e->args[0].get());
                    line("call void @beans_list_reserve(ptr " + recv.first + ", i64 " +
                         capacity.first + ", i64 " + std::to_string(e->line) +
                         ", i64 " + std::to_string(e->col) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "push") {
                    EV v = eval(e->args[0].get(), elem);
                    transfer_in(v);
                    line("call void @beans_list_push(ptr " + recv.first + ", i64 " +
                         to_slot(v, true) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "len") {
                    return {load_at(recv.first, 8, cg.t_i64()), cg.t_i64()};
                }
                if (mname == "pop") {
                    std::string len = load_at(recv.first, 8, cg.t_i64());
                    std::string c = reg();
                    line(c + " = icmp sgt i64 " + len + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("pop", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string n1 = reg();
                    line(n1 + " = sub i64 " + len + ", 1");
                    store_at(recv.first, 8, n1, cg.t_i64());
                    std::string data = load_at(recv.first, 0, cg.t_str());
                    std::string ep = reg(), raw = reg();
                    line(ep + " = getelementptr i64, ptr " + data + ", i64 " + n1);
                    line(raw + " = load i64, ptr " + ep);
                    std::string popped = from_slot(raw, elem, true);
                    own(popped, elem); // moved out of the list
                    std::string sb = make_option_some({popped, elem}, elem);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(elem);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(elem));
                    return {r, cg.t_option(elem)};
                }
                if (mname == "get") {
                    EV idx = eval(e->args[0].get());
                    std::string len = load_at(recv.first, 8, cg.t_i64());
                    std::string c = reg();
                    line(c + " = icmp ult i64 " + idx.first + ", " + len);
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("get", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string data = load_at(recv.first, 0, cg.t_str());
                    std::string ep = reg(), raw = reg();
                    line(ep + " = getelementptr i64, ptr " + data + ", i64 " + idx.first);
                    line(raw + " = load i64, ptr " + ep);
                    std::string sb = make_option_some({from_slot(raw, elem), elem}, elem);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(elem);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(elem));
                    return {r, cg.t_option(elem)};
                }
                if (mname == "max") {
                    int kind = order_kind(elem);
                    std::string okf = fresh_slot("mokf", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_list_max(ptr " + recv.first +
                         ", i64 " + std::to_string(kind) + ", ptr " + okf + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("max", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string sb = make_option_some({from_slot(raw, elem), elem}, elem);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(elem);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(elem));
                    return {r, cg.t_option(elem)};
                }
                if (mname == "contains") {
                    EV v = eval(e->args[0].get(), elem);
                    int kind = eq_kind(elem);
                    std::string c = reg(), r = reg();
                    line(c + " = call i64 @beans_list_contains(ptr " + recv.first +
                         ", i64 " + to_slot(v) + ", i64 " + std::to_string(kind) +
                         ", ptr " + eq_thunk(elem, kind) + ")");
                    line(r + " = icmp ne i64 " + c + ", 0");
                    return {r, cg.t_bool()};
                }
                if (mname == "join") {
                    EV sep = eval(e->args[0].get(), cg.t_str());
                    // element rendering matches the interpreter's display();
                    // flat kinds keep the specialized loop, nested elements
                    // (lists, enums) render through their show helper
                    int kind = elem->k == Ty::str_   ? 2
                               : elem->k == Ty::f64_ ? 1
                               : elem->k == Ty::dec_ ? 3
                               : elem->k == Ty::i1_  ? 4
                               : elem->k == Ty::i64_ ? 0
                                                     : -1;
                    std::string r = reg();
                    if (kind < 0) {
                        std::string es = cg.request_show(elem);
                        line(r + " = call ptr @beans_list_join_show(ptr " +
                             recv.first + ", ptr " + sep.first + ", ptr " + es + ")");
                    } else {
                        line(r + " = call ptr @beans_list_join(ptr " + recv.first +
                             ", ptr " + sep.first + ", i64 " + std::to_string(kind) +
                             ")");
                    }
                    own(r, cg.t_str());
                    return {r, cg.t_str()};
                }
                if (mname == "first" || mname == "last") {
                    std::string len = load_at(recv.first, 8, cg.t_i64());
                    std::string c = reg();
                    line(c + " = icmp sgt i64 " + len + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("fl", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string at = "0";
                    if (mname == "last") {
                        at = reg();
                        line(at + " = sub i64 " + len + ", 1");
                    }
                    std::string data = load_at(recv.first, 0, cg.t_str());
                    std::string ep = reg(), raw = reg();
                    line(ep + " = getelementptr i64, ptr " + data + ", i64 " + at);
                    line(raw + " = load i64, ptr " + ep);
                    std::string sb = make_option_some({from_slot(raw, elem), elem}, elem);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(elem);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(elem));
                    return {r, cg.t_option(elem)};
                }
                if (mname == "min") {
                    int kind = order_kind(elem);
                    std::string okf = fresh_slot("mnok", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_list_min(ptr " + recv.first +
                         ", i64 " + std::to_string(kind) + ", ptr " + okf + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("min", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string sb = make_option_some({from_slot(raw, elem), elem}, elem);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(elem);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(elem));
                    return {r, cg.t_option(elem)};
                }
                if (mname == "index_of") {
                    EV v = eval(e->args[0].get(), elem);
                    int kind = eq_kind(elem);
                    std::string okf = fresh_slot("ixok", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_list_index(ptr " + recv.first +
                         ", i64 " + to_slot(v) + ", i64 " + std::to_string(kind) +
                         ", ptr " + okf + ", ptr " + eq_thunk(elem, kind) + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("iof", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string sb = make_option_some({raw, cg.t_i64()}, cg.t_i64());
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(cg.t_i64());
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(cg.t_i64()));
                    return {r, cg.t_option(cg.t_i64())};
                }
                if (mname == "insert") {
                    EV idx = eval(e->args[0].get());
                    EV v = eval(e->args[1].get(), elem);
                    transfer_in(v);
                    line("call void @beans_list_insert(ptr " + recv.first + ", i64 " +
                         idx.first + ", i64 " + to_slot(v, true) + ", i64 " +
                         std::to_string(e->line) + ", i64 " + std::to_string(e->col) +
                         ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "remove") {
                    EV idx = eval(e->args[0].get());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_list_remove(ptr " + recv.first +
                         ", i64 " + idx.first + ", i64 " + std::to_string(e->line) +
                         ", i64 " + std::to_string(e->col) + ")");
                    std::string v = from_slot(raw, elem, true);
                    if (is_rc(elem)) own(v, elem); // moved out of the list
                    return {v, elem};
                }
                if (mname == "reverse") {
                    line("call void @beans_list_reverse(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "clear") {
                    line("call void @beans_list_clear(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "slice") {
                    EV a = eval(e->args[0].get());
                    EV b2 = eval(e->args[1].get());
                    std::string r = reg();
                    line(r + " = call ptr @beans_list_slice(ptr " + recv.first +
                         ", i64 " + a.first + ", i64 " + b2.first + ", i64 " +
                         std::to_string(e->line) + ", i64 " + std::to_string(e->col) +
                         ")");
                    own(r, rt_);
                    return {r, rt_};
                }
                if (mname == "sort") {
                    int kind = order_kind(elem);
                    line("call void @beans_list_sort(ptr " + recv.first + ", i64 " +
                         std::to_string(kind) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "sort_by") {
                    EV clo = eval(e->args[0].get());
                    std::string thunk = cg.request_sort_thunk(elem);
                    line("call void @beans_list_sort_by(ptr " + recv.first + ", ptr " +
                         thunk + ", ptr " + clo.first + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "sort_by_key") {
                    EV clo = eval(e->args[0].get());
                    std::string thunk = cg.request_sort_key_thunk(elem);
                    line("call void @beans_list_sort_by_key(ptr " + recv.first +
                         ", ptr " + thunk + ", ptr " + clo.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::map_: {
                Ty* K = rt_->args[0];
                Ty* V = rt_->args[1];
                int kind = eq_kind(K);
                std::string keq = eq_thunk(K, kind);
                std::string khash = hash_thunk(K, kind);
                if (mname == "clone") {
                    std::string copy = reg();
                    line(copy + " = call ptr @beans_map_clone(ptr " + recv.first +
                         ", i64 " + std::to_string(kind) + ", ptr " + khash + ")");
                    Ty* result = rt_;
                    own(copy, result);
                    return {copy, result};
                }
                if (mname == "reserve") {
                    EV capacity = eval(e->args[0].get());
                    line("call void @beans_map_reserve(ptr " + recv.first + ", i64 " +
                         capacity.first + ", i64 " + std::to_string(kind) +
                         ", ptr " + khash + ", i64 " + std::to_string(e->line) +
                         ", i64 " + std::to_string(e->col) + ")");
                    return {"", cg.t_unit()};
                }
                if (mname == "set") {
                    EV k = eval(e->args[0].get(), K);
                    EV v = eval(e->args[1].get(), V);
                    transfer_in(k);
                    transfer_in(v);
                    if (kind == 0) {
                        line("call void @beans_map_set_raw(ptr " + recv.first +
                             ", i64 " + to_slot(k, true) + ", i64 " +
                             to_slot(v, true) + ")");
                    } else {
                        line("call void @beans_map_set(ptr " + recv.first + ", i64 " +
                             to_slot(k, true) + ", i64 " + to_slot(v, true) +
                             ", i64 " +
                             std::to_string(kind) + ", ptr " + keq + ", ptr " +
                             khash + ")");
                    }
                    return {"", cg.t_unit()};
                }
                if (mname == "insert") {
                    EV k = eval(e->args[0].get(), K);
                    EV v = eval(e->args[1].get(), V);
                    transfer_in(k);
                    transfer_in(v);
                    std::string raw = reg(), result = reg();
                    if (kind == 0) {
                        line(raw + " = call i64 @beans_map_insert_raw(ptr " +
                             recv.first + ", i64 " + to_slot(k, true) + ", i64 " +
                             to_slot(v, true) + ")");
                    } else {
                        line(raw + " = call i64 @beans_map_insert(ptr " + recv.first +
                             ", i64 " + to_slot(k, true) + ", i64 " +
                             to_slot(v, true) + ", i64 " + std::to_string(kind) +
                             ", ptr " + keq + ", ptr " + khash + ")");
                    }
                    line(result + " = icmp ne i64 " + raw + ", 0");
                    return {result, cg.t_bool()};
                }
                if (mname == "get") {
                    EV k = eval(e->args[0].get(), K);
                    std::string raw = reg();
                    std::string okv = reg(), c = reg();
                    if (kind == 0) {
                        std::string pair = reg();
                        line(pair + " = call {i64, i64} @beans_map_get_raw(ptr " +
                             recv.first + ", i64 " + to_slot(k) + ")");
                        line(raw + " = extractvalue {i64, i64} " + pair + ", 0");
                        line(okv + " = extractvalue {i64, i64} " + pair + ", 1");
                    } else {
                        std::string okf = fresh_slot("gokf", cg.t_i64());
                        line(raw + " = call i64 @beans_map_get(ptr " + recv.first +
                             ", i64 " + to_slot(k) + ", i64 " +
                             std::to_string(kind) + ", ptr " + okf + ", ptr " + keq +
                             ", ptr " + khash + ")");
                        line(okv + " = load i64, ptr " + okf);
                    }
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("mg", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string sb = make_option_some({from_slot(raw, V), V}, V);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(V);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(V));
                    return {r, cg.t_option(V)};
                }
                if (mname == "len") {
                    return {load_at(recv.first, 8, cg.t_i64()), cg.t_i64()};
                }
                if (mname == "contains") {
                    EV k = eval(e->args[0].get(), K);
                    if (kind == 0) {
                        std::string raw = reg(), r = reg();
                        line(raw + " = call i64 @beans_map_contains_raw(ptr " +
                             recv.first + ", i64 " + to_slot(k) + ")");
                        line(r + " = icmp ne i64 " + raw + ", 0");
                        return {r, cg.t_bool()};
                    }
                    std::string okf = fresh_slot("cokf", cg.t_i64());
                    std::string raw = reg();
                    (void)raw;
                    std::string d = reg();
                    line(d + " = call i64 @beans_map_get(ptr " + recv.first + ", i64 " +
                         to_slot(k) + ", i64 " + std::to_string(kind) + ", ptr " + okf +
                         ", ptr " + keq + ", ptr " + khash + ")");
                    std::string okv = reg(), r = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(r + " = icmp ne i64 " + okv + ", 0");
                    return {r, cg.t_bool()};
                }
                if (mname == "remove") {
                    EV k = eval(e->args[0].get(), K);
                    std::string c = reg(), r = reg();
                    if (kind == 0) {
                        line(c + " = call i64 @beans_map_remove_raw(ptr " + recv.first +
                             ", i64 " + to_slot(k) + ")");
                    } else {
                        line(c + " = call i64 @beans_map_remove(ptr " + recv.first +
                             ", i64 " + to_slot(k) + ", i64 " + std::to_string(kind) +
                             ", ptr " + keq + ", ptr " + khash + ")");
                    }
                    line(r + " = icmp ne i64 " + c + ", 0");
                    return {r, cg.t_bool()};
                }
                if (mname == "keys" || mname == "values") {
                    Ty* elem = mname == "keys" ? K : V;
                    std::string r = reg();
                    line(r + " = call ptr @beans_map_" + mname + "(ptr " + recv.first +
                         ")");
                    own(r, cg.t_list(elem));
                    return {r, cg.t_list(elem)};
                }
                if (mname == "clear") {
                    line("call void @beans_map_clear(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::arena_: {
                Ty* inner = rt_->args[0];
                if (mname == "put") {
                    EV value = eval(e->args[0].get(), inner);
                    transfer_in(value);
                    std::string handle = reg();
                    line(handle + " = call i64 @beans_arena_put(ptr " + recv.first +
                         ", i64 " + to_slot(value, true) + ")");
                    return {handle, cg.t_i64()};
                }
                if (mname == "get") {
                    EV handle = eval(e->args[0].get(), cg.t_i64());
                    std::string ok_slot = fresh_slot("arena.ok", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_arena_get(ptr " + recv.first +
                         ", i64 " + handle.first + ", ptr " + ok_slot + ")");
                    std::string ok = reg(), cond = reg();
                    line(ok + " = load i64, ptr " + ok_slot);
                    line(cond + " = icmp ne i64 " + ok + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("arena.option", cg.t_str());
                    line("br i1 " + cond + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string sb = make_option_some({from_slot(raw, inner), inner}, inner);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(inner);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string result = reg();
                    line(result + " = load ptr, ptr " + slot);
                    own(result, cg.t_option(inner));
                    return {result, cg.t_option(inner)};
                }
                if (mname == "at") {
                    EV handle = eval(e->args[0].get(), cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_arena_at(ptr " + recv.first +
                         ", i64 " + handle.first + ", i64 " +
                         std::to_string(e->line) + ", i64 " +
                         std::to_string(e->col) + ")");
                    std::string value = from_slot(raw, inner);
                    if (is_rc(inner)) {
                        emit_retain(value);
                        own(value, inner);
                    }
                    return {value, inner};
                }
                if (mname == "len") {
                    std::string len = reg();
                    line(len + " = call i64 @beans_arena_len(ptr " + recv.first + ")");
                    return {len, cg.t_i64()};
                }
                if (mname == "clear") {
                    line("call void @beans_arena_clear(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::box_: {
                Ty* inner = rt_->args[0];
                if (mname == "get") {
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_box_get(ptr " + recv.first + ")");
                    std::string value = from_slot(raw, inner);
                    if (is_rc(inner)) {
                        emit_retain(value);
                        own(value, inner);
                    }
                    return {value, inner};
                }
                if (mname == "set") {
                    EV value = eval(e->args[0].get(), inner);
                    transfer_in(value);
                    line("call void @beans_box_set(ptr " + recv.first + ", i64 " +
                         to_slot(value, true) + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::shared_: {
                Ty* inner = rt_->args[0];
                if (mname == "get") {
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_shared_get(ptr " + recv.first + ")");
                    std::string value = from_slot(raw, inner);
                    if (is_rc(inner)) {
                        emit_retain(value);
                        own(value, inner);
                    }
                    return {value, inner};
                }
                if (mname == "downgrade") {
                    std::string weak = reg();
                    line(weak + " = call ptr @beans_shared_downgrade(ptr " + recv.first +
                         ")");
                    Ty* result = cg.t_weak(inner);
                    own(weak, result);
                    return {weak, result};
                }
                break;
            }
            case Ty::weak_: {
                Ty* inner = rt_->args[0];
                if (mname == "upgrade") {
                    std::string strong = reg();
                    line(strong + " = call ptr @beans_weak_upgrade(ptr " + recv.first +
                         ")");
                    Ty* result = cg.t_option(cg.t_shared(inner));
                    own(strong, result);
                    return {strong, result};
                }
                if (mname == "expired") {
                    std::string raw = reg(), result = reg();
                    line(raw + " = call i64 @beans_weak_expired(ptr " + recv.first + ")");
                    line(result + " = icmp ne i64 " + raw + ", 0");
                    return {result, cg.t_bool()};
                }
                break;
            }
            case Ty::thread_: {
                if (mname == "join") {
                    Ty* ret = rt_->args[0];
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_thread_join(ptr " + recv.first + ")");
                    if (ret->k == Ty::unit_) return {"", cg.t_unit()};
                    return {from_slot(raw, ret, true), ret};
                }
                break;
            }
            case Ty::mutex_: {
                if (mname == "with") {
                    Ty* inner = rt_->args[0];
                    EV clo = eval(e->args[0].get());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_mutex_lock(ptr " + recv.first + ")");
                    std::string iv = from_slot(raw, inner);
                    std::string fp = load_at(clo.first, 0, cg.t_str());
                    line("call void " + fp + "(ptr " + clo.first + ", " +
                         std::string(ll(inner)) + " " + iv + ")");
                    line("call void @beans_mutex_unlock(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::chan_: {
                Ty* elem = rt_->args[0];
                if (mname == "send") {
                    EV v = eval(e->args[0].get(), elem);
                    transfer_in(v);
                    std::string ok = reg();
                    line(ok + " = call i64 @beans_chan_send(ptr " + recv.first + ", i64 " +
                         to_slot(v, true) + ")");
                    std::string c = reg();
                    line(c + " = icmp ne i64 " + ok + ", 0");
                    std::string okb = bb(), badb = bb();
                    line("br i1 " + c + ", label %" + okb + ", label %" + badb);
                    label(badb);
                    std::string msg = cg.intern_string("send on a closed channel");
                    line("call void @beans_panic(ptr " + msg + ", i64 " +
                         std::to_string(e->line) + ", i64 " + std::to_string(e->col) + ")");
                    line("unreachable");
                    label(okb);
                    return {"", cg.t_unit()};
                }
                if (mname == "recv") {
                    std::string okf = fresh_slot("rokf", cg.t_i64());
                    std::string raw = reg();
                    line(raw + " = call i64 @beans_chan_recv(ptr " + recv.first + ", ptr " +
                         okf + ")");
                    std::string okv = reg(), c = reg();
                    line(okv + " = load i64, ptr " + okf);
                    line(c + " = icmp ne i64 " + okv + ", 0");
                    std::string someb = bb(), noneb = bb(), endb = bb();
                    std::string slot = fresh_slot("rv", cg.t_str());
                    line("br i1 " + c + ", label %" + someb + ", label %" + noneb);
                    label(someb);
                    std::string recvd = from_slot(raw, elem, true);
                    own(recvd, elem); // the queue's ref moves to us
                    std::string sb = make_option_some({recvd, elem}, elem);
                    consume(sb);
                    line("store ptr " + sb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(noneb);
                    std::string nb = make_option_none(elem);
                    consume(nb);
                    line("store ptr " + nb + ", ptr " + slot);
                    line("br label %" + endb);
                    label(endb);
                    std::string r = reg();
                    line(r + " = load ptr, ptr " + slot);
                    own(r, cg.t_option(elem));
                    return {r, cg.t_option(elem)};
                }
                if (mname == "close") {
                    line("call void @beans_chan_close(ptr " + recv.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            case Ty::atomic_: {
                if (mname == "add") {
                    EV v = eval(e->args[0].get());
                    std::string r = reg();
                    line(r + " = call i64 @beans_atomic_add(ptr " + recv.first + ", i64 " +
                         v.first + ")");
                    return {r, cg.t_i64()};
                }
                if (mname == "get") {
                    std::string r = reg();
                    line(r + " = call i64 @beans_atomic_get(ptr " + recv.first + ")");
                    return {r, cg.t_i64()};
                }
                if (mname == "set") {
                    EV v = eval(e->args[0].get());
                    line("call void @beans_atomic_set(ptr " + recv.first + ", i64 " +
                         v.first + ")");
                    return {"", cg.t_unit()};
                }
                break;
            }
            default:
                break;
        }
        err(e, "method '" + mname + "'");
        return {"0", cg.t_i64()};
    }

    // ---- match ----
    EV eval_match(const Expr* e, Ty* hint) {
        EV subj = eval(e->subject.get());
        if (cg.hir.mir().match_subject_needs_pin(e)) pin_borrow(e->subject.get(), subj);
        std::string endb = bb();
        // any block arm means statement position (checker-enforced): the whole
        // match is valueless and expr-arm values die as arm-local temps —
        // merging some arms into the slot while others flip unit leaked them
        bool has_block = false;
        for (const MatchArm& a : e->arms) has_block |= a.is_block;
        Ty* result = has_block ? nullptr : hint;
        bool unit_result = has_block;
        std::string slot;
        // rc slots start null and are cleared after the merge load: a unit arm
        // stores nothing, and a re-entered match (loops) must not release the
        // previous iteration's already-dead pointer
        auto make_slot = [&] {
            slot = fresh_slot("mat", result);
            if (has_owned_refs(result))
                entry_inits += "  store " + std::string(ll(result)) + " " +
                               (is_rc(result) ? "null" : "zeroinitializer") +
                               ", ptr " + slot + "\n";
        };
        if (result && result->k != Ty::unit_) make_slot();

        for (size_t ai = 0; ai < e->arms.size(); ai++) {
            const MatchArm& arm = e->arms[ai];
            std::string armb = bb();
            std::string nextb = ai + 1 < e->arms.size() ? bb() : endb;

            std::string cond = pattern_cond(arm.pat.get(), subj);
            line("br i1 " + cond + ", label %" + armb + ", label %" + nextb);
            label(armb);
            if (arm.is_block) {
                // statement arm: `pattern => { stmts }` — no value to merge.
                // bindings borrow from the subject box, same as expr arms
                scopes.emplace_back();
                bind_pattern(arm.pat.get(), subj);
                exec_block(arm.body);
                scopes.pop_back();
                unit_result = true;
                if (!terminated) line("br label %" + endb);
                if (ai + 1 < e->arms.size()) label(nextb);
                continue;
            }
            scopes.emplace_back();
            bind_pattern(arm.pat.get(), subj);
            size_t amark = temps.size();
            EV v = eval(arm.value.get(), result && !unit_result ? result : nullptr);
            scopes.pop_back();
            if (!result && !unit_result) {
                if (v.second->k == Ty::unit_) unit_result = true;
                else {
                    result = v.second;
                    make_slot();
                }
            }
            if (result && !unit_result && v.second->k != Ty::unit_) {
                if (has_owned_refs(result)) transfer_in(v);
                line("store " + std::string(ll(result)) + " " + v.first + ", ptr " + slot);
            }
            if (!terminated) {
                flush_temps(amark); // arm-local temps die inside the arm
                line("br label %" + endb);
            }
            if (ai + 1 < e->arms.size()) label(nextb);
        }
        label(endb);
        if (unit_result || !result) return {"", cg.t_unit()};
        std::string r = reg();
        line(r + " = load " + std::string(ll(result)) + ", ptr " + slot);
        if (has_owned_refs(result))
            line("store " + std::string(ll(result)) + " " +
                 (is_rc(result) ? "null" : "zeroinitializer") + ", ptr " + slot);
        own(r, result);
        return {r, result};
    }

    std::string pattern_cond(const Pattern* p, const EV& subj) {
        switch (p->kind) {
            case Pattern::Kind::wildcard:
                return "true";
            case Pattern::Kind::alt: {
                std::string acc;
                for (const PatPtr& a : p->alts) {
                    std::string c = pattern_cond(a.get(), subj);
                    if (acc.empty()) {
                        acc = c;
                    } else {
                        std::string r = reg();
                        line(r + " = or i1 " + acc + ", " + c);
                        acc = r;
                    }
                }
                return acc;
            }
            case Pattern::Kind::literal: {
                EV lit = eval(p->lit.get(), subj.second);
                std::string r = reg();
                if (subj.second->k == Ty::str_) {
                    std::string c = reg();
                    line(c + " = call i32 @beans_str_cmp(ptr " + subj.first + ", ptr " +
                         lit.first + ")");
                    line(r + " = icmp eq i32 " + c + ", 0");
                } else if (subj.second->k == Ty::f64_) {
                    line(r + " = fcmp oeq " + std::string(ll(subj.second)) + " " +
                         subj.first + ", " + lit.first);
                } else if (subj.second->k == Ty::i1_) {
                    line(r + " = icmp eq i1 " + subj.first + ", " + lit.first);
                } else if (subj.second->k == Ty::dec_) {
                    std::string c = reg();
                    line(c + " = call i32 @beans_decv_cmp(i128 " + subj.first +
                         ", i128 " + lit.first + ")");
                    line(r + " = icmp eq i32 " + c + ", 0");
                } else {
                    line(r + " = icmp eq " + std::string(ll(subj.second)) + " " +
                         subj.first + ", " + lit.first);
                }
                return r;
            }
            case Pattern::Kind::range: {
                EV lo = eval(p->lit.get(), subj.second);
                EV hi = eval(p->lit2.get(), subj.second);
                std::string a = reg(), b2 = reg(), r = reg();
                std::string ge = subj.second->is_unsigned ? "uge" : "sge";
                std::string end = p->inclusive
                                      ? (subj.second->is_unsigned ? "ule" : "sle")
                                      : (subj.second->is_unsigned ? "ult" : "slt");
                line(a + " = icmp " + ge + " " + std::string(ll(subj.second)) + " " +
                     subj.first + ", " + lo.first);
                line(b2 + " = icmp " + end + " " + std::string(ll(subj.second)) + " " +
                     subj.first + ", " + hi.first);
                line(r + " = and i1 " + a + ", " + b2);
                return r;
            }
            case Pattern::Kind::name: {
                int tag = cg.variant_tag(subj.second->name, p->name);
                if (subj.second->name == "Option" && is_inline_option(subj.second)) {
                    std::string has = option_has(subj);
                    if (tag == 0) return has;
                    std::string none = reg();
                    line(none + " = xor i1 " + has + ", true");
                    return none;
                }
                if (subj.second->name == "Result" && is_inline_result(subj.second)) {
                    std::string ok = result_is_ok(subj);
                    if (tag == 0) return ok;
                    std::string error = reg();
                    line(error + " = xor i1 " + ok + ", true");
                    return error;
                }
                std::string r = reg();
                if (is_niche_option(subj.second)) {
                    line(r + " = icmp " + std::string(tag == 0 ? "ne" : "eq") +
                         " ptr " + subj.first + ", null");
                } else {
                    std::string t = load_at(subj.first, 0, cg.t_i64());
                    line(r + " = icmp eq i64 " + t + ", " + std::to_string(tag));
                }
                return r;
            }
        }
        return "false";
    }

    std::vector<Ty*> payload_types(const std::string& enum_name, const std::string& variant,
                                   const std::vector<Ty*>& subj_args,
                                   uint32_t line_, uint32_t col_) {
        if (enum_name == "Option") {
            return variant == "some" && !subj_args.empty() ? std::vector<Ty*>{subj_args[0]}
                                                           : std::vector<Ty*>{};
        }
        if (enum_name == "Result") {
            if (variant == "ok" && !subj_args.empty()) return {subj_args[0]};
            if (variant == "err" && subj_args.size() >= 2) return {subj_args[1]};
            return {};
        }
        std::vector<Ty*> out;
        if (const EnumVariant* v = cg.variant_decl(enum_name, variant)) {
            for (const Param& p : v->payload) {
                out.push_back(cg.resolve(p.type.get(), CG2::empty_env(), line_, col_));
            }
        }
        return out;
    }

    void bind_pattern(const Pattern* p, const EV& subj) {
        if (p->kind != Pattern::Kind::name || p->bindings.empty()) return;
        std::vector<Ty*> ptys =
            payload_types(subj.second->name, p->name, subj.second->args, p->line, p->col);
        for (size_t i = 0; i < p->bindings.size() && i < ptys.size(); i++) {
            std::string v;
            if (subj.second->name == "Option" && i == 0) {
                v = option_payload(subj, ptys[i]);
            } else if (subj.second->name == "Result" && i == 0) {
                v = result_payload(subj, p->name == "ok");
            } else {
                v = load_slot_at(subj.first, 8 + 8 * static_cast<int>(i), ptys[i]);
            }
            std::string slot = alloc_slot(p->bindings[i].name, ptys[i]);
            line("store " + std::string(ll(ptys[i])) + " " + v + ", ptr " + slot);
        }
    }

    // ---- statements ----
    void exec_block(const std::vector<StmtPtr>& stmts) {
        scopes.emplace_back();
        for (const StmtPtr& s : stmts) {
            if (terminated) break;
            size_t mark = temps.size();
            exec(s.get());
            if (!terminated) flush_temps(mark);
            // a terminating statement (return/break) released every temp on
            // its own path — drop its entries, or the sibling branch's next
            // flush re-releases values it never made (LLVM's dominance error)
            else if (temps.size() > mark) temps.resize(mark);
        }
        if (!terminated) release_scopes(scopes.size() - 1);
        scopes.pop_back();
    }

    void exec(const Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::let_: {
                Ty* t = rt(s->type.get(), s->line, s->col);
                if (t->k == Ty::bad_ || t->k == Ty::unit_) return;
                bool borrow = false;
                if (has_owned_refs(t) && s->init && s->init->kind == Expr::Kind::ident) {
                    std::string src(s->init->text);
                    Var* sv = find_var(src);
                    borrow = sv && !sv->boxed && !boxed_names.count(s->name) &&
                             !taken_names.count(s->name) &&
                             !ever_assigned(src) && !ever_assigned(s->name);
                }
                EV v = eval(s->init.get(), t);
                if (!borrow) transfer_in(v);
                alloc_slot(s->name, t);
                Var* var = find_var(s->name);
                var->owned = !borrow && (has_owned_refs(t) || var->boxed);
                if (t->k == Ty::dec_)
                    var->decimal_scale = known_decimal_scale(s->init.get());
                if (s->init && s->init->kind == Expr::Kind::closure &&
                    !ever_assigned(s->name)) {
                    auto known = closure_symbols.find(s->init.get());
                    if (known != closure_symbols.end()) var->direct_fn = known->second;
                }
                line("store " + std::string(ll(t)) + " " + v.first + ", ptr " +
                     var_ptr(var));
                if (!var->live_flag.empty())
                    line("store i1 " + std::string(var->owned ? "1" : "0") +
                         ", ptr " + var->live_flag);
                break;
            }
            case Stmt::Kind::assign: {
                std::string ptr;
                Ty* t = nullptr;
                Var* local_var = nullptr;
                if (s->target->kind == Expr::Kind::ident) {
                    Var* var = find_var(std::string(s->target->text));
                    if (!var) { cg.err(s->line, s->col, "this assignment"); return; }
                    local_var = var;
                    var->direct_fn.clear();
                    ptr = var_ptr(var);
                    t = var->ty;
                } else if (s->target->kind == Expr::Kind::field) {
                    auto [p, ft] = field_ptr(s->target.get());
                    ptr = p;
                    t = ft;
                } else if (s->target->kind == Expr::Kind::index) {
                    exec_index_assign(s);
                    return;
                } else {
                    cg.err(s->line, s->col, "assigning here");
                    return;
                }
                EV v = eval(s->value.get(), t);
                if (s->op == TokenKind::assign) {
                    if (local_var && t->k == Ty::dec_)
                        local_var->decimal_scale = -1;
                    if (has_owned_refs(t)) {
                        transfer_in(v);
                        std::string old = reg();
                        line(old + " = load " + std::string(ll(t)) + ", ptr " + ptr);
                        line("store " + std::string(ll(t)) + " " + v.first + ", ptr " + ptr);
                        if (local_var && !local_var->live_flag.empty()) {
                            std::string live = reg();
                            line(live + " = load i1, ptr " + local_var->live_flag);
                            std::string drop = bb(), done = bb();
                            line("br i1 " + live + ", label %" + drop + ", label %" +
                                 done);
                            label(drop);
                            emit_release_value(old, t);
                            line("br label %" + done);
                            label(done);
                            line("store i1 1, ptr " + local_var->live_flag);
                        } else {
                            emit_release_value(old, t);
                        }
                    } else {
                        line("store " + std::string(ll(t)) + " " + v.first + ", ptr " + ptr);
                    }
                    return;
                }
                std::string cur = reg();
                line(cur + " = load " + std::string(ll(t)) + ", ptr " + ptr);
                std::string r = reg();
                if (t->k == Ty::dec_) {
                    Decimal literal;
                    if ((s->op == TokenKind::plus_eq ||
                         s->op == TokenKind::minus_eq) &&
                        decimal_literal_value(s->value.get(), literal)) {
                        const int known_scale =
                            local_var ? local_var->decimal_scale : -1;
                        EV updated = decimal_literal_add(
                            {cur, t}, v, literal,
                            s->op == TokenKind::minus_eq,
                            s->value->line, s->value->col,
                            known_scale == literal.scale);
                        if (local_var && known_scale >= 0 &&
                            literal.scale > known_scale)
                            local_var->decimal_scale = -1;
                        line("store i128 " + updated.first + ", ptr " + ptr);
                        return;
                    }
                    if (local_var) local_var->decimal_scale = -1;
                    const char* fn = s->op == TokenKind::plus_eq    ? "add"
                                     : s->op == TokenKind::minus_eq ? "sub"
                                     : s->op == TokenKind::star_eq  ? "mul"
                                                                    : "div";
                    if (s->op == TokenKind::slash_eq) {
                        line(r + " = call i128 @beans_decv_div(i128 " + cur +
                             ", i128 " + v.first + ", i64 " +
                             std::to_string(s->value->line) +
                             ", i64 " + std::to_string(s->value->col) + ")");
                    } else {
                        line(r + " = call i128 @beans_decv_" + fn + "(i128 " + cur +
                             ", i128 " + v.first + ")");
                    }
                    line("store i128 " + r + ", ptr " + ptr);
                    return;
                } else {
                    bool flt = t->k == Ty::f64_ || t->k == Ty::simd4f32_;
                    const char* op = nullptr;
                    switch (s->op) {
                        case TokenKind::plus_eq: op = flt ? "fadd" : "add"; break;
                        case TokenKind::minus_eq: op = flt ? "fsub" : "sub"; break;
                        case TokenKind::star_eq: op = flt ? "fmul" : "mul"; break;
                        case TokenKind::slash_eq:
                            op = flt ? "fdiv" : t->is_unsigned ? "udiv" : "sdiv";
                            break;
                        case TokenKind::percent_eq:
                            op = flt ? "frem" : t->is_unsigned ? "urem" : "srem";
                            break;
                        default: return;
                    }
                    if (!flt && (s->op == TokenKind::slash_eq ||
                                 s->op == TokenKind::percent_eq)) {
                        guard_div_zero(v.first, t, s->op == TokenKind::percent_eq,
                                       s->value->line, s->value->col);
                    }
                    line(r + " = " + op + " " + std::string(ll(t)) + " " + cur +
                         ", " + v.first);
                    r = flt ? normalize_float(r, t) : normalize_integer(r, t);
                }
                line("store " + std::string(ll(t)) + " " + r + ", ptr " + ptr);
                break;
            }
            case Stmt::Kind::expr:
                eval(s->expr.get());
                break;
            case Stmt::Kind::defer_: {
                // armed flag: the deferred call runs at exit only if this
                // statement was actually reached (evaluated at exit, like the
                // interpreter does)
                std::string flag = fresh_slot("dfl", cg.t_bool());
                entry_inits += "  store i1 0, ptr " + flag + "\n";
                line("store i1 1, ptr " + flag);
                defers.push_back({s->expr.get(), flag, scopes});
                break;
            }
            case Stmt::Kind::ret: {
                if (is_main) {
                    emit_ret("ret i32 0");
                } else if (s->expr) {
                    EV v = eval(s->expr.get(), ret_ty);
                    std::string moved_slot;
                    if (has_owned_refs(v.second) &&
                        s->expr->kind == Expr::Kind::ident) {
                        Var* local = find_var(std::string(s->expr->text));
                        if (local && local->owned && !local->boxed)
                            moved_slot = local->slot;
                    }
                    if (has_owned_refs(v.second) && moved_slot.empty() &&
                        !consume(v.first))
                        emit_retain_value(v.first, v.second);
                    emit_ret("ret " + std::string(ll(v.second)) + " " + v.first,
                             "", moved_slot);
                } else {
                    emit_ret("ret void");
                }
                terminated = true;
                break;
            }
            case Stmt::Kind::brk:
                if (!loop_stack.empty()) {
                    for (const EV& t : temps) emit_release_value(t.first, t.second);
                    release_scopes(loop_stack.back().scope_depth);
                    line("br label %" + loop_stack.back().brk);
                    terminated = true;
                }
                break;
            case Stmt::Kind::cont:
                if (!loop_stack.empty()) {
                    for (const EV& t : temps) emit_release_value(t.first, t.second);
                    release_scopes(loop_stack.back().scope_depth);
                    line("br label %" + loop_stack.back().cont);
                    terminated = true;
                }
                break;
            case Stmt::Kind::if_: {
                EV c = eval(s->cond.get());
                std::string then_bb = bb();
                std::string else_bb = s->else_body.empty() ? "" : bb();
                std::string end_bb = bb();
                line("br i1 " + c.first + ", label %" + then_bb + ", label %" +
                     (else_bb.empty() ? end_bb : else_bb));
                label(then_bb);
                exec_block(s->body);
                if (!terminated) line("br label %" + end_bb);
                if (!else_bb.empty()) {
                    label(else_bb);
                    exec_block(s->else_body);
                    if (!terminated) line("br label %" + end_bb);
                }
                label(end_bb);
                break;
            }
            case Stmt::Kind::for_ever: {
                std::string head = bb(), end = bb();
                line("br label %" + head);
                label(head);
                loop_stack.push_back({end, head, scopes.size()});
                exec_block(s->body);
                loop_stack.pop_back();
                if (!terminated) line("br label %" + head);
                label(end);
                break;
            }
            case Stmt::Kind::for_while: {
                std::string head = bb(), body_bb = bb(), end = bb();
                line("br label %" + head);
                label(head);
                size_t cmark = temps.size();
                EV c = eval(s->cond.get());
                flush_temps(cmark);
                line("br i1 " + c.first + ", label %" + body_bb + ", label %" + end);
                label(body_bb);
                loop_stack.push_back({end, head, scopes.size()});
                exec_block(s->body);
                loop_stack.pop_back();
                if (!terminated) line("br label %" + head);
                label(end);
                break;
            }
            case Stmt::Kind::for_in:
                exec_for_in(s);
                break;
            case Stmt::Kind::unsafe_:
                exec_block(s->body);
                break;
            default:
                cg.err(s->line, s->col, "this statement");
                break;
        }
    }

    void exec_for_in(const Stmt* s) {
        if (s->iterable && s->iterable->kind == Expr::Kind::range) {
            EV lo = eval(s->iterable->lhs.get());
            EV hi = eval(s->iterable->rhs.get(), lo.second);
            Ty* elem = lo.second;
            scopes.emplace_back();
            alloc_slot(s->loop_var, elem);
            Var* lv = find_var(s->loop_var);
            if (lv->boxed) lv->owned = true;
            line("store " + std::string(ll(elem)) + " " + lo.first + ", ptr " + var_ptr(lv));
            std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
            line("br label %" + head);
            label(head);
            std::string cur = var_read(lv), c = reg();
            std::string pred = s->iterable->inclusive
                                   ? (elem->is_unsigned ? "ule" : "sle")
                                   : (elem->is_unsigned ? "ult" : "slt");
            line(c + " = icmp " + pred + " " + std::string(ll(elem)) + " " + cur +
                 ", " + hi.first);
            line("br i1 " + c + ", label %" + body_bb + ", label %" + end);
            label(body_bb);
            loop_stack.push_back({end, step, scopes.size()});
            exec_block(s->body);
            loop_stack.pop_back();
            if (!terminated) line("br label %" + step);
            label(step);
            std::string cur2 = var_read(lv), nxt = reg();
            line(nxt + " = add " + std::string(ll(elem)) + " " + cur2 + ", 1");
            line("store " + std::string(ll(elem)) + " " + nxt + ", ptr " + var_ptr(lv));
            line("br label %" + head);
            label(end);
            scopes.pop_back();
            return;
        }
        // list iteration by index
        EV it = eval(s->iterable.get());
        if (it.second->k == Ty::slice_) {
            Ty* element = it.second->args[0];
            std::string pointer = reg(), length = reg();
            line(pointer + " = extractvalue {ptr, i64} " + it.first + ", 0");
            line(length + " = extractvalue {ptr, i64} " + it.first + ", 1");
            scopes.emplace_back();
            std::string idx = fresh_slot("idx", cg.t_i64());
            line("store i64 0, ptr " + idx);
            alloc_slot(s->loop_var, element);
            Var* loop_value = find_var(s->loop_var);
            std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
            line("br label %" + head);
            label(head);
            std::string index = reg(), condition = reg();
            line(index + " = load i64, ptr " + idx);
            line(condition + " = icmp slt i64 " + index + ", " + length);
            line("br i1 " + condition + ", label %" + body_bb + ", label %" + end);
            label(body_bb);
            std::string item_pointer = reg(), item = reg();
            line(item_pointer + " = getelementptr " + std::string(ll(element)) +
                 ", ptr " + pointer + ", i64 " + index);
            line(item + " = load " + std::string(ll(element)) + ", ptr " +
                 item_pointer + ", align 1");
            line("store " + std::string(ll(element)) + " " + item + ", ptr " +
                 var_ptr(loop_value));
            loop_stack.push_back({end, step, scopes.size()});
            exec_block(s->body);
            loop_stack.pop_back();
            if (!terminated) line("br label %" + step);
            label(step);
            std::string old_index = reg(), next_index = reg();
            line(old_index + " = load i64, ptr " + idx);
            line(next_index + " = add i64 " + old_index + ", 1");
            line("store i64 " + next_index + ", ptr " + idx);
            line("br label %" + head);
            label(end);
            scopes.pop_back();
            return;
        }
        if (it.second->k == Ty::fixed_array_) {
            Ty* elem = it.second->args[0];
            std::string storage = fresh_slot("arrayiter", it.second);
            line("store " + std::string(ll(it.second)) + " " + it.first +
                 ", ptr " + storage);
            scopes.emplace_back();
            std::string idx = fresh_slot("idx", cg.t_i64());
            line("store i64 0, ptr " + idx);
            alloc_slot(s->loop_var, elem);
            Var* lv = find_var(s->loop_var);
            std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
            line("br label %" + head);
            label(head);
            std::string i = reg(), condition = reg();
            line(i + " = load i64, ptr " + idx);
            line(condition + " = icmp slt i64 " + i + ", " +
                 std::to_string(it.second->array_len));
            line("br i1 " + condition + ", label %" + body_bb + ", label %" + end);
            label(body_bb);
            std::string pointer = reg(), item = reg();
            line(pointer + " = getelementptr " + std::string(ll(it.second)) +
                 ", ptr " + storage + ", i64 0, i64 " + i);
            line(item + " = load " + std::string(ll(elem)) + ", ptr " + pointer);
            line("store " + std::string(ll(elem)) + " " + item + ", ptr " +
                 var_ptr(lv));
            loop_stack.push_back({end, step, scopes.size()});
            exec_block(s->body);
            loop_stack.pop_back();
            if (!terminated) line("br label %" + step);
            label(step);
            std::string old_index = reg(), next_index = reg();
            line(old_index + " = load i64, ptr " + idx);
            line(next_index + " = add i64 " + old_index + ", 1");
            line("store i64 " + next_index + ", ptr " + idx);
            line("br label %" + head);
            label(end);
            scopes.pop_back();
            return;
        }
        if (it.second->k != Ty::list_) {
            cg.err(s->line, s->col, "looping over this");
            return;
        }
        Ty* elem = it.second->args[0];
        scopes.emplace_back();
        std::string idx = fresh_slot("idx", cg.t_i64());
        line("store i64 0, ptr " + idx);
        alloc_slot(s->loop_var, elem);
        Var* lv = find_var(s->loop_var);
        if (lv->boxed) lv->owned = true;
        std::string head = bb(), body_bb = bb(), step = bb(), end = bb();
        line("br label %" + head);
        label(head);
        std::string i = reg(), len = load_at(it.first, 8, cg.t_i64()), c = reg();
        line(i + " = load i64, ptr " + idx);
        line(c + " = icmp slt i64 " + i + ", " + len);
        line("br i1 " + c + ", label %" + body_bb + ", label %" + end);
        label(body_bb);
        std::string data = load_at(it.first, 0, cg.t_str());
        std::string ep = reg(), raw = reg();
        line(ep + " = getelementptr i64, ptr " + data + ", i64 " + i);
        line(raw + " = load i64, ptr " + ep);
        std::string elem_val = from_slot(raw, elem);
        if (lv->boxed && is_rc(elem)) {
            std::string cellp = var_ptr(lv);
            std::string old = reg();
            line(old + " = load ptr, ptr " + cellp);
            emit_retain(elem_val);
            line("store " + std::string(ll(elem)) + " " + elem_val + ", ptr " + cellp);
            emit_release(old);
        } else {
            line("store " + std::string(ll(elem)) + " " + elem_val + ", ptr " +
                 var_ptr(lv));
        }
        loop_stack.push_back({end, step, scopes.size()});
        exec_block(s->body);
        loop_stack.pop_back();
        if (!terminated) line("br label %" + step);
        label(step);
        std::string i2 = reg(), n2 = reg();
        line(i2 + " = load i64, ptr " + idx);
        line(n2 + " = add i64 " + i2 + ", 1");
        line("store i64 " + n2 + ", ptr " + idx);
        line("br label %" + head);
        label(end);
        scopes.pop_back();
    }

    // ---- whole function ----
    std::string emit() {
        cg.cur_pkg = pkg; // plain names in this body resolve here
        ret_ty = cg.resolve(ret_ref, env, dline, dcol);

        // which locals do closures inside this body capture? those get heap cells
        {
            ClosureScan scan;
            scan.block(body_ref);
            boxed_names = std::move(scan.captured);
            taken_names = std::move(scan.taken);
        }
        {
            ReadScan scan;
            if (has_self) scan.declarations["self"] += 1;
            for (const Param& param : params_ref) scan.declarations[param.name] += 1;
            if (captured) {
                for (const auto& [name, type] : *captured) {
                    (void)type;
                    scan.declarations[name] += 1;
                }
            }
            scan.block(body_ref);
            remaining_reads = std::move(scan.reads);
            declaration_counts = std::move(scan.declarations);
        }

        Ty* self_ty = nullptr;
        if (has_self) {
            if (self_impl) self_ty = cg.t_obj(self_impl->mangled);
            else if (self_iface) self_ty = cg.t_obj(self_iface->qualname, self_iface);
            else if (self_enum) self_ty = cg.t_enum(self_enum->qualname, {});
        }
        if (fn_name == "deinit" && self_impl) {
            for (CImpl* p = self_impl->parent; p; p = p->parent) {
                bool has = false;
                for (const FnDecl& m : p->decl->methods) {
                    if (m.has_self && m.name == "deinit" && m.has_body) has = true;
                }
                if (has) { deinit_chain = "@m_" + p->mangled + "_deinit"; break; }
            }
        }

        std::string header = "define ";
        header += is_main ? "i32" : ll(ret_ty);
        header += " " + symbol + "(";
        scopes.emplace_back();

        bool first = true;
        if (captured) {
            header += "ptr %env";
            first = false;
        }
        if (self_ty) {
            if (!first) header += ", ";
            header += "ptr %self.arg";
            first = false;
            std::string slot = alloc_slot("self", self_ty, true);
            bool bx = boxed_names.count("self") > 0;
            store_param("%self.arg", slot, "ptr", bx, self_ty, false);
            if (bx) scopes.back()["self"].owned = true; // the frame made the cell
        }
        for (size_t i = 0; i < params_ref.size(); i++) {
            Ty* pt = cg.resolve(params_ref[i].type.get(), env,
                                params_ref[i].line, params_ref[i].col);
            if (!first) header += ", ";
            first = false;
            std::string preg = "%p" + std::to_string(i);
            if (params_ref[i].passing == Param::Passing::inout) {
                header += "ptr " + preg;
                scopes.back()[params_ref[i].name] = {
                    preg, "", pt, false, false, true, "", next_seq++, loop_stack.size()};
                continue;
            }
            header += std::string(ll(pt)) + " " + preg;
            std::string slot = alloc_slot(params_ref[i].name, pt, true);
            bool bx = boxed_names.count(params_ref[i].name) > 0;
            bool takes = params_ref[i].passing == Param::Passing::take;
            store_param(preg, slot, ll(pt), bx, pt, takes);
            Var& param_var = scopes.back()[params_ref[i].name];
            if (bx || takes) param_var.owned = true;
            if (takes && !bx && has_owned_refs(pt) && !param_var.live_flag.empty()) {
                entry_inits += "  store i1 1, ptr " + param_var.live_flag + "\n";
            }
        }
        header += ") {\nentry:\n";

        // captured cells arrive behind %env: {fnptr @0, cell0 @8, cell1 @16, ...}
        if (captured) {
            for (size_t i = 0; i < captured->size(); i++) {
                const auto& [name, ty] = (*captured)[i];
                std::string slot = "%v" + std::to_string(next_reg++) + "." + name;
                allocas += "  " + slot + " = alloca ptr\n";
                std::string cp = "%cap" + std::to_string(i);
                entry_inits += "  " + cp + " = getelementptr i8, ptr %env, i64 " +
                               std::to_string(8 + 8 * i) + "\n";
                std::string cell = cp + ".c";
                entry_inits += "  " + cell + " = load ptr, ptr " + cp + "\n";
                entry_inits += "  store ptr " + cell + ", ptr " + slot + "\n";
                scopes.back()[name] = {slot, "", ty, true, false, false, "", next_seq++,
                                       loop_stack.size()};
            }
        }

        std::string firstb = bb();
        // The function body's scope is NOT released at block end the way
        // nested blocks are: emit_ret runs the armed defers first and then
        // releases the whole frame, so a defer never runs on freed locals.
        // (threads.b's `defer ch.close()` closed a freed channel before this.)
        scopes.emplace_back();
        for (const StmtPtr& s : body_ref) {
            if (terminated) break;
            size_t mark = temps.size();
            exec(s.get());
            if (!terminated) flush_temps(mark);
            else if (temps.size() > mark) temps.resize(mark); // see exec_block
        }
        if (!terminated) {
            if (is_main) emit_ret("ret i32 0");
            else if (ret_ty->k == Ty::unit_) emit_ret("ret void");
            else if (ret_ty->k == Ty::f64_) emit_ret("ret " + std::string(ll(ret_ty)) +
                                                      " " + fmt_double(0));
            else if (ret_ty->k == Ty::simd4f32_)
                emit_ret("ret <4 x float> zeroinitializer");
            else if (ret_ty->k == Ty::fixed_array_)
                emit_ret("ret " + std::string(ll(ret_ty)) + " zeroinitializer");
            else if (ret_ty->k == Ty::struct_)
                emit_ret("ret " + std::string(ll(ret_ty)) + " zeroinitializer");
            else if (ret_ty->k == Ty::slice_)
                emit_ret("ret {ptr, i64} zeroinitializer");
            else if (is_inline_option(ret_ty) || is_inline_result(ret_ty))
                emit_ret("ret " + std::string(ll(ret_ty)) + " zeroinitializer");
            else if (ret_ty->k == Ty::i64_ || ret_ty->k == Ty::i1_ ||
                     ret_ty->k == Ty::dec_)
                emit_ret("ret " + std::string(ll(ret_ty)) + " 0");
            else emit_ret("ret ptr null");
        }
        scopes.pop_back();
        scopes.pop_back();

        return header + allocas + entry_inits + "  br label %" + firstb + "\n" +
               firstb + ":\n" + body + "}\n\n";
    }

    // param → its slot; boxed params store into their heap cell instead.
    // the cell owns its content, so RC params get a +1 going in.
    void store_param(const std::string& preg, const std::string& slot,
                     const std::string& lty, bool boxed, Ty* type, bool takes) {
        if (!boxed) {
            entry_inits += "  store " + lty + " " + preg + ", ptr " + slot + "\n";
            return;
        }
        std::string cell = preg + ".cell";
        entry_inits += "  " + cell + " = load ptr, ptr " + slot + "\n";
        if (has_owned_refs(type) && !takes) emit_retain_value_entry(preg, type);
        entry_inits += "  store " + lty + " " + preg + ", ptr " + cell + "\n";
    }
};

// ---- CodeGen driver ---------------------------------------------------------

CodeGen::CodeGen(const HirProgram& hir) : hir_(hir), prog_(hir.ast()) {}

void CodeGen::error_at(uint32_t line, uint32_t col, std::string msg) {
    errors_.push_back({std::move(msg), line, col});
}

std::string CodeGen::generate() {
    CG2 cg(hir_, errors_);

    for (const auto& pkg : prog_.packages) {
        for (const auto& pf : pkg->files) {
            // top-level functions (generic ones are emitted on demand)
            for (const FnDecl& f : pf->mod.fns) {
                if (f.is_extern_c) {
                    cg.cur_pkg = pkg->prefix;
                    cg.request_extern(&f);
                    continue;
                }
                if (!f.generics.empty()) continue;
                bool main = f.qualname == "main";
                FnEmit fe(cg, f, main ? "@main" : "@b_" + f.qualname, main,
                          nullptr, nullptr, nullptr, CG2::empty_env());
                fe.pkg = pkg->prefix;
                cg.fn_text += fe.emit();
            }

            // enum methods
            for (const EnumDecl& e : pf->mod.enums) {
                for (const FnDecl& m : e.methods) {
                    FnEmit fe(cg, m, "@em_" + e.qualname + "_" + m.name, false,
                              nullptr, nullptr, &e, CG2::empty_env());
                    fe.pkg = pkg->prefix;
                    cg.fn_text += fe.emit();
                }
            }

            // interface default methods (emitted once per interface)
            for (const ClassDecl& c : pf->mod.classes) {
                if (!c.is_interface) continue;
                for (const FnDecl& m : c.methods) {
                    if (!m.has_body || !m.has_self) continue;
                    FnEmit fe(cg, m, "@m_" + c.qualname + "_" + m.name, false,
                              nullptr, &c, nullptr, CG2::empty_env());
                    fe.pkg = pkg->prefix;
                    cg.fn_text += fe.emit();
                }
            }
        }
    }

    // class methods per instantiation + generic fn instances — both queues can
    // grow while we emit, so drain them together
    std::set<std::string> emitted;
    while (true) {
        CImpl* im = nullptr;
        for (CImpl* q : cg.impl_queue) {
            if (!emitted.count(q->mangled)) { im = q; break; }
        }
        if (im) {
            emitted.insert(im->mangled);
            for (const FnDecl& m : im->decl->methods) {
                if (!m.has_body) continue;
                std::string sym = (m.has_self ? "@m_" : "@s_") + im->mangled + "_" + m.name;
                FnEmit fe(cg, m, sym, false, im, nullptr, nullptr, im->env);
                fe.pkg = CG2::pkg_of(im->decl->qualname);
                cg.fn_text += fe.emit();
            }
            continue;
        }
        size_t fidx = cg.fn_queue.size();
        for (size_t i = 0; i < cg.fn_queue.size(); i++) {
            if (!cg.fn_emitted.count(cg.fn_queue[i].symbol)) { fidx = i; break; }
        }
        if (fidx == cg.fn_queue.size()) break;
        // copy out — the queue vector can reallocate while this fn emits
        const FnDecl* fdecl = cg.fn_queue[fidx].decl;
        std::map<std::string, Ty*> fenv = cg.fn_queue[fidx].env;
        std::string fsym = cg.fn_queue[fidx].symbol;
        cg.fn_emitted.insert(fsym);
        FnEmit fe(cg, *fdecl, fsym, false, nullptr, nullptr, nullptr, fenv);
        fe.pkg = CG2::pkg_of(fdecl->qualname);
        cg.fn_text += fe.emit();
    }

    if (!errors_.empty()) return "";
    ffi_c_ = std::move(cg.ffi_c);

    // deinit dispatches from the C runtime through the vtable, so its slot
    // must exist even though beans code can never call it by name
    bool any_deinit = false;
    for (const auto& up : cg.impls) {
        for (const FnDecl& m : up->decl->methods) {
            if (m.has_self && m.name == "deinit" && m.has_body) any_deinit = true;
        }
    }
    if (any_deinit) cg.selector("deinit");

    // Static descriptors: class id first, then the global selector table
    // inline. Objects carry one descriptor pointer, so dispatch needs the same
    // two dependent loads as a C++ vptr call.
    int nsel = static_cast<int>(cg.selectors.size());
    std::string tables;
    for (const auto& up : cg.impls) {
        CImpl* im = up.get();
        int slots = nsel > 0 ? nsel : 1;
        tables += "@td_" + im->mangled + " = internal constant {i64, [" +
                  std::to_string(slots) + " x ptr]} {i64 " +
                  std::to_string(im->id) + ", [" + std::to_string(slots) +
                  " x ptr] [";
        for (int s = 0; s < slots; s++) {
            if (s) tables += ", ";
            std::string sym = "null";
            for (const auto& [name, idx] : cg.selectors) {
                if (idx != s) continue;
                CG2::FoundMethod fm = cg.find_method_class(im, name, true);
                if (fm.decl) sym = "@m_" + fm.owner + "_" + name;
            }
            tables += "ptr " + sym;
        }
        tables += "]}\n";
    }
    // vtable slot the C runtime dispatches deinit through; -1 = program has none
    tables += "@beans_deinit_sel = global i64 " +
              std::to_string(any_deinit ? cg.selectors["deinit"] : -1) + "\n";
    // class parent table for as?
    tables += "@beans_class_parents = global [" +
              std::to_string(cg.impls.empty() ? 1 : cg.impls.size()) + " x i64] [";
    if (cg.impls.empty()) {
        tables += "i64 -1";
    } else {
        for (size_t i = 0; i < cg.impls.size(); i++) {
            if (i) tables += ", ";
            tables += "i64 " +
                      std::to_string(cg.impls[i]->parent ? cg.impls[i]->parent->id : -1);
        }
    }
    tables += "]\n";

    std::string out;
    out += "; generated by beansc\n";
    out += cg.type_defs;
    if (!cg.type_defs.empty()) out += "\n";
    out += "declare ptr @beans_alloc(i64, i64)\n";
    out += "declare ptr @beans_raw_alloc(i64, i64, i64, i64)\n";
    out += "declare void @beans_raw_free(ptr)\n";
    out += "declare void @beans_raw_copy(ptr, ptr, i64, i64, i64, i64)\n";
    out += "declare void @beans_raw_zero(ptr, i64, i64, i64, i64)\n";
    out += "declare void @beans_retain(ptr)\n";
    out += "declare void @beans_release(ptr)\n";
    out += "declare ptr @beans_from_int(i64)\n";
    out += "declare ptr @beans_from_uint(i64)\n";
    out += "declare ptr @beans_from_float(double)\n";
    out += "declare ptr @beans_from_bool(i32)\n";
    out += "declare ptr @beans_concat(ptr, ptr)\n";
    out += "declare ptr @beans_interpolate(i64, ...)\n";
    out += "declare void @beans_println(ptr)\n";
    out += "declare void @beans_print(ptr)\n";
    out += "declare i32 @beans_str_cmp(ptr, ptr)\n";
    // registry rows declare themselves — one row, one declare, one C symbol
    auto irty = [](BT t) -> const char* {
        switch (t) {
            case BT::unit: return "void";
            case BT::self_recv: return "void";
            case BT::f64: return "double";
            case BT::dec: return "i128";
            case BT::str: return "ptr";
            case BT::bytes: return "ptr";
            case BT::file: return "ptr";
            case BT::mmap: return "ptr";
            case BT::list_str: return "ptr";
            case BT::opt_i64:
            case BT::opt_str: return "{i64, i64}";
            case BT::res_i64:
            case BT::res_f64:
            case BT::res_dec:
            case BT::res_str:
            case BT::res_bool:
            case BT::res_bytes:
            case BT::res_file:
            case BT::res_mmap:
            case BT::res_list_str: return "{i64, ptr}";
            default: return "i64"; // i64, boolean
        }
    };
    for (const BuiltinMethod& b : builtin_methods()) {
        out += "declare " + std::string(irty(b.ret)) + " @" + b.sym + "(ptr";
        for (BT p : b.params) out += std::string(", ") + irty(p);
        if (b.panics) out += ", i64, i64";
        out += ")\n";
    }
    for (const BuiltinStatic& b : builtin_statics()) {
        out += "declare " + std::string(irty(b.ret)) + " @" + b.sym + "(";
        for (size_t i = 0; i < b.params.size(); i++) {
            if (i) out += ", ";
            out += irty(b.params[i]);
        }
        if (b.panics) out += std::string(b.params.empty() ? "" : ", ") + "i64, i64";
        out += ")\n";
    }
    for (const BuiltinFn& b : builtin_fns()) {
        out += "declare " + std::string(irty(b.ret)) + " @" + b.sym + "(";
        for (size_t i = 0; i < b.params.size(); i++) {
            if (i) out += ", ";
            out += irty(b.params[i]);
        }
        if (b.panics) out += std::string(b.params.empty() ? "" : ", ") + "i64, i64";
        out += ")\n";
    }
    out += "declare void @beans_eprintln(ptr)\n";
    out += "declare void @beans_eprint(ptr)\n";
    out += "declare i64 @beans_bytes_eq(ptr, ptr)\n";
    out += "declare void @beans_panic(ptr, i64, i64)\n";
    out += "declare void @beans_panic_index(i64, i64, i64, i64, i64)\n";
    out += "declare void @beans_panic_array_index(i64, i64, i64, i64)\n";
    out += "declare void @beans_panic_slice_index(i64, i64, i64, i64)\n";
    out += "declare i64 @beans_is_a(i64, i64)\n";
    out += "declare ptr @beans_box_new(i64, i64)\n";
    out += "declare i64 @beans_box_get(ptr)\n";
    out += "declare void @beans_box_set(ptr, i64)\n";
    out += "declare ptr @beans_shared_new(i64, i64)\n";
    out += "declare i64 @beans_shared_get(ptr)\n";
    out += "declare ptr @beans_shared_downgrade(ptr)\n";
    out += "declare ptr @beans_weak_upgrade(ptr)\n";
    out += "declare i64 @beans_weak_expired(ptr)\n";
    out += "declare ptr @beans_arena_new(i64, i64, i64, i64)\n";
    out += "declare i64 @beans_arena_put(ptr, i64)\n";
    out += "declare i64 @beans_arena_get(ptr, i64, ptr)\n";
    out += "declare i64 @beans_arena_at(ptr, i64, i64, i64)\n";
    out += "declare i64 @beans_arena_len(ptr)\n";
    out += "declare void @beans_arena_clear(ptr)\n";
    out += "declare ptr @beans_list_new(i64)\n";
    out += "declare ptr @beans_list_clone(ptr)\n";
    out += "declare ptr @beans_list_join(ptr, ptr, i64)\n";
    out += "declare ptr @beans_show_list(ptr, ptr)\n";
    out += "declare ptr @beans_show_run(ptr, i64)\n";
    out += "declare void @beans_show_append(ptr, ptr)\n";
    out += "declare void @beans_show_push_val(ptr, ptr, i64)\n";
    out += "declare void @beans_show_push_lit(ptr, ptr)\n";
    out += "declare void @beans_show_list_iter(ptr, ptr, ptr)\n";
    out += "declare ptr @beans_list_join_show(ptr, ptr, ptr)\n";
    out += "declare void @beans_list_push(ptr, i64)\n";
    out += "declare void @beans_list_reserve(ptr, i64, i64, i64)\n";
    out += "declare i64 @beans_list_min(ptr, i64, ptr)\n";
    out += "declare i64 @beans_list_index(ptr, i64, i64, ptr, ptr)\n";
    out += "declare i64 @beans_str_eq(ptr, ptr)\n";
    out += "declare void @beans_list_insert(ptr, i64, i64, i64, i64)\n";
    out += "declare i64 @beans_list_remove(ptr, i64, i64, i64)\n";
    out += "declare void @beans_list_reverse(ptr)\n";
    out += "declare void @beans_list_clear(ptr)\n";
    out += "declare ptr @beans_list_slice(ptr, i64, i64, i64, i64)\n";
    out += "declare void @beans_list_sort(ptr, i64)\n";
    out += "declare void @beans_list_sort_by(ptr, ptr, ptr)\n";
    out += "declare void @beans_list_sort_by_key(ptr, ptr, ptr)\n";
    out += "declare i64 @beans_map_remove(ptr, i64, i64, ptr, ptr)\n";
    out += "declare i64 @beans_map_remove_raw(ptr, i64)\n";
    out += "declare ptr @beans_map_keys(ptr)\n";
    out += "declare ptr @beans_map_values(ptr)\n";
    out += "declare void @beans_map_clear(ptr)\n";
    out += "declare i64 @beans_slot_mix(i64)\n";
    out += "declare i64 @beans_f64_hash(i64)\n";
    out += "declare i64 @beans_f32_hash(i64)\n";
    out += "declare i64 @beans_str_hash(ptr)\n";
    out += "declare i64 @beans_dec_hash(ptr)\n";
    out += "declare i64 @beans_bytes_hash(ptr)\n";
    out += "declare i64 @beans_f64_round(double)\n";
    out += "declare double @llvm.fabs.f64(double)\n";
    out += "declare float @llvm.fabs.f32(float)\n";
    out += "declare i32 @beans_dec_cmp(ptr, ptr)\n";
    out += "declare ptr @beans_dec_str(ptr)\n";
    out += "declare ptr @beans_decv_box(i128)\n";
    out += "declare i128 @beans_decv_unbox(ptr)\n";
    out += "declare i128 @beans_decv_add(i128, i128)\n";
    out += "declare i128 @beans_decv_sub(i128, i128)\n";
    out += "declare i128 @beans_decv_mul(i128, i128)\n";
    out += "declare i128 @beans_decv_div(i128, i128, i64, i64)\n";
    out += "declare i128 @beans_decv_neg(i128)\n";
    out += "declare i128 @beans_decv_abs(i128)\n";
    out += "declare i128 @beans_decv_round(i128, i64)\n";
    out += "declare i32 @beans_decv_cmp(i128, i128)\n";
    out += "declare ptr @beans_decv_str(i128)\n";
    out += "declare i128 @beans_decv_from_int(i64)\n";
    out += "declare i128 @beans_decv_from_f64(double)\n";
    out += "declare i64 @beans_decv_to_int(i128)\n";
    out += "declare double @beans_decv_to_f64(i128)\n";
    out += "declare i64 @beans_list_max(ptr, i64, ptr)\n";
    out += "declare i64 @beans_list_contains(ptr, i64, i64, ptr)\n";
    out += "declare ptr @beans_map_new(i64, i64, i64)\n";
    out += "declare ptr @beans_map_clone(ptr, i64, ptr)\n";
    out += "declare void @beans_map_reserve(ptr, i64, i64, ptr, i64, i64)\n";
    out += "declare void @beans_map_set(ptr, i64, i64, i64, ptr, ptr)\n";
    out += "declare i64 @beans_map_insert(ptr, i64, i64, i64, ptr, ptr)\n";
    out += "declare i64 @beans_map_get(ptr, i64, i64, ptr, ptr, ptr)\n";
    out += "declare void @beans_map_set_raw(ptr, i64, i64)\n";
    out += "declare i64 @beans_map_insert_raw(ptr, i64, i64)\n";
    out += "declare {i64, i64} @beans_map_get_raw(ptr, i64)\n";
    out += "declare i64 @beans_map_contains_raw(ptr, i64)\n";
    out += "declare ptr @beans_thread_spawn(ptr, ptr)\n";
    out += "declare i64 @beans_thread_join(ptr)\n";
    out += "declare ptr @beans_mutex_new(i64, i64)\n";
    out += "declare i64 @beans_mutex_lock(ptr)\n";
    out += "declare void @beans_mutex_unlock(ptr)\n";
    out += "declare ptr @beans_chan_new(i64, i64)\n";
    out += "declare i64 @beans_chan_send(ptr, i64)\n";
    out += "declare i64 @beans_chan_recv(ptr, ptr)\n";
    out += "declare void @beans_chan_close(ptr)\n";
    out += "declare ptr @beans_atomic_new(i64)\n";
    out += "declare i64 @beans_atomic_add(ptr, i64)\n";
    out += "declare i64 @beans_atomic_get(ptr)\n";
    out += "declare void @beans_atomic_set(ptr, i64)\n\n";
    out += cg.globals;
    out += "\n";
    out += tables;
    out += "\n";
    out += cg.fn_text;
    out += cg.lifted;
    return out;
}

const char* CodeGen::runtime_c() {
    return R"RT(// beans native runtime — reference-counted heap + cycle collector.
// Every heap value has a 16-byte header just before its payload:
//   { long long rc, long long meta }   (rc ops turn atomic at first spawn)
// meta bits 0-2 = kind, bits 3-60 = per-kind shape payload:
//   0 leaf | 1 fixed (bitmask of pointer slots) | 2 list (elem_ptr)
//   3 map (key_ptr | val_ptr<<1) | 4 chan (elem_ptr) | 5 mutex (inner_ptr)
//   6 OS resource (shape bit 0: 0 = file — drop closes the fd,
//                               1 = mmap — drop unmaps)
//   7 arena (elem_ptr)
// meta bits 61-62 = collector color, bit 63 = in the root buffer.
// String constants carry an immortal header emitted by the compiler.
//
// Cycles: plain RC can't free A<->B. The collector is Bacon-Rajan trial
// deletion (Nim's ORC family): a decrement that doesn't reach zero parks the
// object as a possible cycle root; a collection trial-deletes each root's
// subgraph, restores whatever still has external counts, frees the rest.
// It only runs when no worker threads are live (checked in beans_alloc and
// at exit), so the mutator is exactly one thread: ourselves, between
// statements. Everything is iterative — a million-node ring must not
// overflow the C stack.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BEANS_IMMORTAL (1LL << 62)

// rc layout: bits 0-47 the count, bits 48-59 the allocation size class
// (0 = plain malloc), bit 62 immortal. Retain/release preserve the class
// bits by adding/subtracting 1; every test of the COUNT must mask with
// RC_COUNT, and class 4095 * 16 bytes stays far under the immortal bit.
#define RC_CLS_SHIFT 48
#define RC_CLS_MAX 4095LL
#define RC_COUNT(v) ((v) & ((1LL << RC_CLS_SHIFT) - 1))
// rc bit 61: this object's class chain has a deinit — user code runs when the
// count hits zero. Lives in the rc word (not meta) so pointer-mask walkers and
// shell frees never see it; retain/release arithmetic can't carry into it.
#define RC_FIN (1LL << 61)

// meta layout
#define CC_SHAPE ((1LL << 61) - 1)
#define CC_COLOR (3LL << 61)
#define CC_BLACK 0LL
#define CC_GRAY (1LL << 61)
#define CC_WHITE (2LL << 61)
#define CC_PURPLE (3LL << 61)
#define CC_BUF ((long long)(1ULL << 63))

typedef struct {
    long long rc;
    long long meta;
} BHead;

static BHead* head_of(void* p) { return (BHead*)((char*)p - 16); }

// counts are plain until the first thread spawns (cc_mt flips before
// pthread_create, so no object is ever touched by two threads while the
// flag is 0); after that retain/release use atomic ops. The collector
// keeps plain ops either way — it only runs with zero workers live.
static int cc_mt;
static int cc_is_mt(void) {
    return __atomic_load_n(&cc_mt, __ATOMIC_RELAXED);
}
static void cc_enable_mt(void) {
    __atomic_store_n(&cc_mt, 1, __ATOMIC_RELAXED);
}
static long long cc_color(BHead* h) { return h->meta & CC_COLOR; }
static void cc_set_color(BHead* h, long long c) { h->meta = (h->meta & ~CC_COLOR) | c; }

static _Atomic long long cc_threads;  // live worker threads; collect only at 0
static _Atomic int cc_pending;
static int cc_collecting;
static void cc_collect(int force);

// vtable slot of deinit, emitted by codegen (-1 when no class has one).
// Deinit runs inside a release cascade, where allocation used to be
// impossible — beans_in_deinit keeps the collector out of that window,
// because a mid-destroy object must never be walked.
extern long long beans_deinit_sel;
// NOT thread-local: a TLS read compiles to a _tlv_get_addr call. A shared
// flag is exactly as correct — the collector only runs with zero worker
// threads, so "any thread is mid-deinit" is the right gate anyway. Plain
// int + __atomic builtins (an _Atomic type rejects __atomic_add_fetch).
static int beans_in_deinit;

// Profile builds recompile the emitted runtime with -DBEANS_ARC_STATS.
// Normal benchmark binaries do not contain these counters, so measuring
// ownership traffic cannot change the timed result.
#ifdef BEANS_ARC_STATS
static unsigned long long arc_allocations;
static unsigned long long arc_allocated_bytes;
static unsigned long long arc_retain_calls;
static unsigned long long arc_release_calls;
static unsigned long long arc_release_nodes;
static unsigned long long arc_freed_shells;
static unsigned long long arc_possible_roots;
static unsigned long long arc_collections;
static unsigned long long arc_cycle_objects;
#define ARC_ADD(name, value) \
    __atomic_add_fetch(&(name), (unsigned long long)(value), __ATOMIC_RELAXED)
static void arc_report(void) {
    fprintf(stderr,
            "beans arc stats: allocations=%llu allocated_bytes=%llu "
            "retains=%llu releases=%llu release_nodes=%llu frees=%llu "
            "possible_roots=%llu collections=%llu cycle_objects=%llu\n",
            (unsigned long long)arc_allocations,
            (unsigned long long)arc_allocated_bytes,
            (unsigned long long)arc_retain_calls,
            (unsigned long long)arc_release_calls,
            (unsigned long long)arc_release_nodes,
            (unsigned long long)arc_freed_shells,
            (unsigned long long)arc_possible_roots,
            (unsigned long long)arc_collections,
            (unsigned long long)arc_cycle_objects);
}
__attribute__((constructor)) static void arc_setup(void) { atexit(arc_report); }
#else
#define ARC_ADD(name, value) ((void)0)
#endif

// deinit, before the children go — outlined and cold: the indirect call must
// stay out of beans_release's hot loop or the optimizer treats every
// iteration as clobbered (that cost 50% on the churn bench). Count up to 1
// and FIN off first: user code in there may retain and release self without
// re-entering death, and death can't run twice (husk and collector paths see
// FIN already gone). Count back to 0 after: the husk filter frees a parked
// shell only when RC_COUNT is 0, so the bump must not outlive the call (it
// leaked a buffered object's shell once).
__attribute__((noinline, cold, preserve_most)) static void beans_do_deinit(
    void* p, BHead* h, long long nrc) {
    if (cc_is_mt()) __atomic_store_n(&h->rc, (nrc + 1) & ~RC_FIN, __ATOMIC_RELAXED);
    else h->rc = (nrc + 1) & ~RC_FIN;
    void** descriptor = *(void***)p;
    void (**methods)(void*) = (void (**)(void*))descriptor;
    __atomic_add_fetch(&beans_in_deinit, 1, __ATOMIC_RELAXED);
    methods[beans_deinit_sel + 1](p);
    __atomic_sub_fetch(&beans_in_deinit, 1, __ATOMIC_RELAXED);
    if (cc_is_mt()) __atomic_store_n(&h->rc, nrc & ~RC_FIN, __ATOMIC_RELAXED);
    else h->rc = nrc & ~RC_FIN;
}

// segregated per-thread freelists over 64KB slabs: one calloc per slab,
// then carve; a free pushes the block on the freeing thread's list. Slabs
// are registered globally so the leaks tool sees every allocation as
// reachable; blocks stranded on a dead worker's freelist sit inside a
// registered slab (wasted until exit, never a leak). BEANS_NO_POOL=1
// routes everything through plain calloc/free so `leaks` can see
// individual beans objects again when hunting a real leak.
#define POOL_CLASSES 64 // pooled sizes 16..1008 bytes; bigger goes to malloc
#define POOL_SLAB (64 << 10)
static _Thread_local void* pool_free[POOL_CLASSES];
static _Thread_local char* pool_cur;
static _Thread_local char* pool_end;
static void** pool_slabs;
static long long pool_slab_len, pool_slab_cap;
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static int pool_off;
__attribute__((constructor)) static void pool_setup(void) {
    pool_off = getenv("BEANS_NO_POOL") != NULL;
}

void beans_panic(const char* msg, long long line, long long col);
void* beans_alloc(long long size, long long meta) {
    ARC_ADD(arc_allocations, 1);
    ARC_ADD(arc_allocated_bytes, size);
    // allocation is the one safe point: never inside a release cascade,
    // and every stored reference is already counted (a deinit body is the
    // exception — cc_collect itself bails while one runs, so this exact
    // condition stays byte-identical to keep clang's fast-path layout)
    if (cc_pending && !cc_collecting && cc_threads == 0) cc_collect(0);
    size_t total = (16 + (size_t)size + 15) & ~(size_t)15;
    long long cls = (long long)(total >> 4);
    BHead* h;
    if (cls < POOL_CLASSES && !pool_off) {
        if (pool_free[cls]) {
            h = pool_free[cls];
            pool_free[cls] = *(void**)h;
            memset(h, 0, total); // recycled block; callers expect zeroed slots
        } else {
            if (!pool_cur || pool_cur + total > pool_end) {
                pool_cur = calloc(1, POOL_SLAB);
                if (!pool_cur) beans_panic("out of memory", 0, 0);
                pool_end = pool_cur + POOL_SLAB;
                pthread_mutex_lock(&pool_mu);
                if (pool_slab_len == pool_slab_cap) {
                    pool_slab_cap = pool_slab_cap ? pool_slab_cap * 2 : 64;
                    pool_slabs = realloc(pool_slabs,
                                         (size_t)pool_slab_cap * sizeof(void*));
                }
                pool_slabs[pool_slab_len++] = pool_cur;
                pthread_mutex_unlock(&pool_mu);
            }
            h = (BHead*)pool_cur; // virgin slab memory, already zero
            pool_cur += total;
        }
        h->rc = 1 | (cls << RC_CLS_SHIFT);
    } else {
        h = calloc(1, total);
        if (!h) beans_panic("out of memory", 0, 0);
        h->rc = 1;
    }
    h->meta = meta;
    return (char*)h + 16;
}

void beans_retain(void* p) {
    if (!p) return;
    ARC_ADD(arc_retain_calls, 1);
    BHead* h = head_of(p);
    if (cc_is_mt()) {
        if (__atomic_load_n(&h->rc, __ATOMIC_RELAXED) >= BEANS_IMMORTAL) return;
        __atomic_add_fetch(&h->rc, 1, __ATOMIC_RELAXED);
    } else {
        if (h->rc >= BEANS_IMMORTAL) return;
        h->rc += 1;
    }
}

void beans_release(void* p);

typedef struct {
    long long* data;
    long long len, cap;
} BList;
typedef struct {
    long long* data;
    long long len, cap;
} BArena;
typedef struct {
    long long* data; // key,value interleaved — len stays at
    long long len, cap; // offset 8: map.len() is a direct field load in IR
    // open-addressed index over data: (hash hi32 << 32) | (pos+2), 0 empty,
    // 1 tombstone. NULL until the map outgrows a linear scan.
    unsigned long long* idx;
    long long icap, tombs;
    // OrderedMap removal leaves stable holes. Plain Map swap-removes and keeps
    // used == len. deadbits NULL means there are no holes.
    long long used;
    unsigned long long* deadbits;
    long long ordered;
} BMap;
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t can_send, can_recv;
    long long* q;
    long long head, count, cap;
    int closed;
} BChan;
typedef struct {
    pthread_mutex_t m;
    long long inner;
} BMutex;

// one shape-walker for destruction and all collector phases
static void cc_walk(void* p, long long meta, void (*fn)(void*, void*), void* ctx) {
    long long kind = meta & 7;
    long long extra = (meta & CC_SHAPE) >> 3;
    if (kind == 1) { // fixed: marked 8-byte slots
        for (int i = 0; i < 58 && (extra >> i); i++) {
            if ((extra >> i) & 1) {
                void* c = *(void**)((char*)p + 8 * i);
                if (c) fn(c, ctx);
            }
        }
    } else if (kind == 2) {
        BList* l = p;
        if (extra & 1) {
            for (long long i = 0; i < l->len; i++) {
                if (l->data[i]) fn((void*)l->data[i], ctx);
            }
        }
    } else if (kind == 3) {
        BMap* m = p;
        for (long long i = 0; i < m->used; i++) { // holes are zeroed: null-skip
            if ((extra & 1) && m->data[i * 2]) fn((void*)m->data[i * 2], ctx);
            if ((extra & 2) && m->data[i * 2 + 1]) fn((void*)m->data[i * 2 + 1], ctx);
        }
    } else if (kind == 4) {
        BChan* c = p;
        if (extra & 1) {
            for (long long i = 0; i < c->count; i++) {
                long long v = c->q[(c->head + i) % c->cap];
                if (v) fn((void*)v, ctx);
            }
        }
    } else if (kind == 5) {
        BMutex* mu = p;
        if ((extra & 1) && mu->inner) fn((void*)mu->inner, ctx);
    } else if (kind == 7) {
        BArena* arena = p;
        if (extra & 1) {
            for (long long i = 0; i < arena->len; i++) {
                if (arena->data[i]) fn((void*)arena->data[i], ctx);
            }
        }
    }
}

typedef struct {
    long long fd;
    long long closed;
} BFile;
typedef struct {
    char* p;
    long long len;
    long long fd;
    long long writable;
    long long closed;
} BMMap;

typedef struct {
    long long strong;
    long long weak;
    long long value;
    long long value_ptr;
} BSharedCtrl;
typedef struct { BSharedCtrl* ctrl; } BSharedHandle;

// kind 6, extra 2 = strong handle, extra 3 = weak handle. The control block
// owns exactly one payload reference while any strong handle exists. Return
// that child when the final strong shell dies so beans_release can keep its
// existing iterative cascade.
static void* shared_shell_drop(void* p, long long extra) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    if (extra & 1) {
        if (__atomic_fetch_sub(&ctrl->weak, 1, __ATOMIC_ACQ_REL) == 1) free(ctrl);
        return NULL;
    }
    void* child = NULL;
    if (__atomic_fetch_sub(&ctrl->strong, 1, __ATOMIC_ACQ_REL) == 1) {
        if (ctrl->value_ptr) child = (void*)ctrl->value;
        ctrl->value = 0;
        if (__atomic_fetch_sub(&ctrl->weak, 1, __ATOMIC_ACQ_REL) == 1) free(ctrl);
    }
    return child;
}

// free the box and its side allocations WITHOUT touching child refs
static void* cc_free_shell(void* p, long long meta) {
    ARC_ADD(arc_freed_shells, 1);
    long long kind = meta & 7;
    long long extra = (meta & CC_SHAPE) >> 3;
    void* deferred_child = NULL;
    if (kind == 2) free(((BList*)p)->data);
    else if (kind == 7) free(((BArena*)p)->data);
    else if (kind == 3) {
        free(((BMap*)p)->data);
        free(((BMap*)p)->idx);
        free(((BMap*)p)->deadbits);
    } else if (kind == 4) free(((BChan*)p)->q);
    else if (kind == 6 && (extra & 2)) {
        deferred_child = shared_shell_drop(p, extra);
    } else if (kind == 6) { // OS resource — dropping the last ref is the safety
        // close whatever is still open at the OS level, whether the handle was
        // never closed or its close was deferred while threads ran (fd/p left
        // valid, the logical `closed` flag already set). The last ref is gone,
        // so no thread can be mid-op here — releasing now is safe.
        if ((meta & CC_SHAPE) >> 3 & 1) { // shape bit 0: 0 = file, 1 = mmap
            BMMap* m = p;
            if (m->p) munmap(m->p, (size_t)m->len);
            if (m->fd >= 0) close((int)m->fd);
        } else {
            BFile* f = p; // net; close() / f.close() is the real API
            if (f->fd >= 0) close((int)f->fd);
        }
    }
    BHead* h = head_of(p);
    long long cls = (h->rc >> RC_CLS_SHIFT) & RC_CLS_MAX;
    if (cls) {
        *(void**)h = pool_free[cls];
        pool_free[cls] = h;
    } else {
        free(h);
    }
    return deferred_child;
}

// explicit work stack, shared by release cascades and all collector phases
typedef struct {
    void** v;
    long long len, cap;
    void** local;
} CCStack;
static void cc_push(CCStack* s, void* p) {
    if (s->len == s->cap) {
        long long next = s->cap ? s->cap * 2 : 4096;
        void** grown = malloc((size_t)next * sizeof(void*));
        if (s->len) memcpy(grown, s->v, (size_t)s->len * sizeof(void*));
        if (s->v != s->local) free(s->v);
        s->v = grown;
        s->cap = next;
    }
    s->v[s->len++] = p;
}
static void cc_visit_push(void* c, void* ctx) {
    BHead* h = head_of(c);
    long long rc = cc_is_mt() ? __atomic_load_n(&h->rc, __ATOMIC_RELAXED) : h->rc;
    if (rc >= BEANS_IMMORTAL) return;
    cc_push(ctx, c);
}

// Release cascades overwhelmingly walk fixed class objects. Keep this path
// direct: the collector still uses the generic callback walker, but ordinary
// ARC death should not pay an indirect call for every child.
static inline void cc_release_children(void* p, long long meta, CCStack* st) {
    long long kind = meta & 7;
    long long extra = (meta & CC_SHAPE) >> 3;
    if (kind != 1) {
        cc_walk(p, meta, cc_visit_push, st);
        return;
    }
    for (int i = 0; i < 58 && (extra >> i); i++) {
        if (!((extra >> i) & 1)) continue;
        void* child = *(void**)((char*)p + 8 * i);
        if (!child) continue;
        BHead* h = head_of(child);
        long long rc = cc_is_mt() ? __atomic_load_n(&h->rc, __ATOMIC_RELAXED)
                                  : h->rc;
        if (rc < BEANS_IMMORTAL) cc_push(st, child);
    }
}

// ---- possible-root buffer ----
static void** cc_roots;
static long long cc_len, cc_cap;
static long long cc_threshold = 256;
static pthread_mutex_t cc_mu = PTHREAD_MUTEX_INITIALIZER;

static void cc_possible_root(void* p) {
    BHead* h = head_of(p);
    long long old = __atomic_fetch_or(&h->meta, CC_PURPLE | CC_BUF, __ATOMIC_RELAXED);
    if (old & CC_BUF) return; // already parked
    ARC_ADD(arc_possible_roots, 1);
    // like the counts, the buffer goes unlocked until the first spawn: one
    // thread exists, and after cc_mt flips every park takes the mutex
    if (cc_is_mt()) pthread_mutex_lock(&cc_mu);
    if (cc_len == cc_cap) {
        cc_cap = cc_cap ? cc_cap * 2 : 1024;
        cc_roots = realloc(cc_roots, (size_t)cc_cap * sizeof(void*));
    }
    cc_roots[cc_len++] = p;
    if (cc_len >= cc_threshold) cc_pending = 1;
    if (cc_is_mt()) pthread_mutex_unlock(&cc_mu);
}

// iterative: a dropped million-node chain pushes children on an explicit
// stack instead of recursing the C stack. The stack stays empty (no malloc)
// unless a death actually cascades.
void beans_release(void* p) {
    if (!p) return;
    ARC_ADD(arc_release_calls, 1);
    void* local[64];
    CCStack st = {local, 0, 64, local};
    void* cur = p;
    for (;;) {
        ARC_ADD(arc_release_nodes, 1);
        BHead* h = head_of(cur);
        long long rc0 = cc_is_mt() ? __atomic_load_n(&h->rc, __ATOMIC_RELAXED) : h->rc;
        if (rc0 < BEANS_IMMORTAL) {
            long long nrc = cc_is_mt() ? __atomic_sub_fetch(&h->rc, 1, __ATOMIC_ACQ_REL)
                                  : (h->rc -= 1);
            if (RC_COUNT(nrc) == 0) {
                long long meta = h->meta;
                // FIN is only ever set on class objects, so it alone decides
                if (__builtin_expect(nrc & RC_FIN, 0)) {
                    beans_do_deinit(cur, h, nrc);
                    meta = h->meta; // colors can move while user code runs
                }
                cc_release_children(cur, meta, &st);
                if (meta & CC_BUF) {
                    // parked — the buffer still points here, so the collector
                    // frees the shell later; mark black: this is a dead husk
                    __atomic_and_fetch(&h->meta, ~CC_COLOR, __ATOMIC_RELAXED);
                } else {
                    void* child = cc_free_shell(cur, meta);
                    if (child) cc_push(&st, child);
                }
            } else {
                // could this shape sit on a cycle? leaves, pointer-free
                // containers, and objects with an empty pointer mask never can
                // — a cycle member needs an outgoing edge — which keeps
                // int-field churn off the buffer
                long long meta = h->meta;
                long long kind = meta & 7;
                int cyclic = (kind == 1 && ((meta & CC_SHAPE) >> 3) != 0) ||
                             (kind == 3 && (meta & (3LL << 3))) ||
                             ((kind == 2 || kind == 4 || kind == 5 || kind == 7) &&
                              (meta & (1LL << 3)));
                if (cyclic) cc_possible_root(cur);
            }
        }
        if (!st.len) break;
        cur = st.v[--st.len];
    }
    if (st.v != local) free(st.v);
}

// Box owns one slot. Checked Beans treats the outer handle as move-only; the
// RC header is still used for uniform frame cleanup and cycle walking.
void* beans_raw_alloc(long long count, long long size, long long line, long long col) {
    if (count < 0) beans_panic("negative raw allocation count", line, col);
    if (size <= 0 || count > (1LL << 58) / size)
        beans_panic("raw allocation too large", line, col);
    void* p = calloc((size_t)count, (size_t)size);
    if (!p && count) beans_panic("out of memory", line, col);
    return p;
}
void beans_raw_free(void* p) { free(p); }
void beans_raw_copy(void* destination, void* source, long long count, long long size,
                    long long line, long long col) {
    if (count < 0) beans_panic("negative raw copy count", line, col);
    if (size <= 0 || count > (1LL << 58) / size)
        beans_panic("raw copy too large", line, col);
    if (count && (!destination || !source))
        beans_panic("null raw pointer copy", line, col);
    memmove(destination, source, (size_t)count * (size_t)size);
}
void beans_raw_zero(void* destination, long long count, long long size,
                    long long line, long long col) {
    if (count < 0) beans_panic("negative raw zero count", line, col);
    if (size <= 0 || count > (1LL << 58) / size)
        beans_panic("raw zero too large", line, col);
    if (count && !destination) beans_panic("null raw pointer zero", line, col);
    memset(destination, 0, (size_t)count * (size_t)size);
}

void* beans_box_new(long long value, long long value_ptr) {
    long long* box = beans_alloc(8, 1 | (value_ptr << 3));
    box[0] = value;
    return box;
}
long long beans_box_get(void* p) { return ((long long*)p)[0]; }
void beans_box_set(void* p, long long value) {
    long long* box = p;
    if (((head_of(p)->meta & CC_SHAPE) >> 3) & 1) {
        void* old = (void*)box[0];
        if (old) beans_release(old);
    }
    box[0] = value;
}

void* beans_shared_new(long long value, long long value_ptr) {
    BSharedCtrl* ctrl = calloc(1, sizeof(BSharedCtrl));
    if (!ctrl) beans_panic("out of memory", 0, 0);
    ctrl->strong = 1;
    ctrl->weak = 1; // implicit weak held until strong reaches zero
    ctrl->value = value;
    ctrl->value_ptr = value_ptr;
    BSharedHandle* handle = beans_alloc(sizeof(BSharedHandle), 6 | (2LL << 3));
    handle->ctrl = ctrl;
    return handle;
}
long long beans_shared_get(void* p) {
    return ((BSharedHandle*)p)->ctrl->value;
}
void* beans_shared_downgrade(void* p) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    __atomic_add_fetch(&ctrl->weak, 1, __ATOMIC_RELAXED);
    BSharedHandle* weak = beans_alloc(sizeof(BSharedHandle), 6 | (3LL << 3));
    weak->ctrl = ctrl;
    return weak;
}
void* beans_weak_upgrade(void* p) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    long long strong = __atomic_load_n(&ctrl->strong, __ATOMIC_ACQUIRE);
    while (strong > 0) {
        if (__atomic_compare_exchange_n(&ctrl->strong, &strong, strong + 1, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            BSharedHandle* handle =
                beans_alloc(sizeof(BSharedHandle), 6 | (2LL << 3));
            handle->ctrl = ctrl;
            return handle;
        }
    }
    return NULL;
}
long long beans_weak_expired(void* p) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    return __atomic_load_n(&ctrl->strong, __ATOMIC_ACQUIRE) == 0;
}

void* beans_arena_new(long long capacity, long long elem_ptr,
                      long long line, long long col) {
    if (capacity < 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "negative arena capacity %lld", capacity);
        beans_panic(msg, line, col);
    }
    if (capacity > (1LL << 58)) beans_panic("arena capacity too large", line, col);
    BArena* arena = beans_alloc(sizeof(BArena), 7 | (elem_ptr << 3));
    if (capacity) {
        arena->data = calloc((size_t)capacity, sizeof(long long));
        if (!arena->data) beans_panic("out of memory", line, col);
    }
    arena->cap = capacity;
    return arena;
}
long long beans_arena_put(void* p, long long value) {
    BArena* arena = p;
    if (arena->len == arena->cap) {
        long long next = arena->cap ? arena->cap * 2 : 8;
        long long* data = realloc(arena->data, (size_t)next * sizeof(long long));
        if (!data) beans_panic("out of memory", 0, 0);
        arena->data = data;
        arena->cap = next;
    }
    long long handle = arena->len++;
    arena->data[handle] = value;
    return handle;
}
long long beans_arena_get(void* p, long long handle, long long* ok) {
    BArena* arena = p;
    if (handle < 0 || handle >= arena->len) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return arena->data[handle];
}
long long beans_arena_at(void* p, long long handle, long long line, long long col) {
    BArena* arena = p;
    if (handle < 0 || handle >= arena->len) {
        char msg[112];
        snprintf(msg, sizeof msg, "arena handle %lld out of range (len %lld)",
                 handle, arena->len);
        beans_panic(msg, line, col);
    }
    return arena->data[handle];
}
long long beans_arena_len(void* p) { return ((BArena*)p)->len; }
void beans_arena_clear(void* p) {
    BArena* arena = p;
    if (((head_of(p)->meta & CC_SHAPE) >> 3) & 1) {
        for (long long i = 0; i < arena->len; i++) {
            if (arena->data[i]) beans_release((void*)arena->data[i]);
        }
    }
    arena->len = 0;
}

// ---- the collector (single mutator: us) ----

static void cc_visit_dec_push(void* c, void* ctx) {
    BHead* h = head_of(c);
    if (h->rc >= BEANS_IMMORTAL) return;
    h->rc -= 1; // trial deletion: one decrement per internal edge
    cc_push(ctx, c);
}
static void cc_mark_gray(void* root, CCStack* st) {
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        BHead* h = head_of(p);
        if (cc_color(h) == CC_GRAY) continue;
        cc_set_color(h, CC_GRAY);
        cc_walk(p, h->meta, cc_visit_dec_push, st);
    }
}

static void cc_visit_inc_push(void* c, void* ctx) {
    BHead* h = head_of(c);
    if (h->rc >= BEANS_IMMORTAL) return;
    h->rc += 1; // undo the trial deletion along this edge
    if (cc_color(h) != CC_BLACK) {
        cc_set_color(h, CC_BLACK);
        cc_push(ctx, c);
    }
}
static void cc_scan_black(void* root, CCStack* st) {
    cc_set_color(head_of(root), CC_BLACK);
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        cc_walk(p, head_of(p)->meta, cc_visit_inc_push, st);
    }
}

static void cc_scan(void* root, CCStack* st, CCStack* aux) {
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        BHead* h = head_of(p);
        if (cc_color(h) != CC_GRAY) continue;
        if (RC_COUNT(h->rc) > 0) {
            cc_scan_black(p, aux); // externally referenced — restore it all
        } else {
            cc_set_color(h, CC_WHITE);
            cc_walk(p, h->meta, cc_visit_push, st);
        }
    }
}

static void cc_collect_white(void* root, CCStack* st, CCStack* dead) {
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        BHead* h = head_of(p);
        if (cc_color(h) != CC_WHITE || (h->meta & CC_BUF)) continue;
        cc_set_color(h, CC_BLACK); // visited; prevents duplicate frees
        cc_walk(p, h->meta, cc_visit_push, st);
        cc_push(dead, p);
    }
}

static long long cc_walk_min = 256; // adaptive gate for trial deletion

static void cc_collect(int force) {
    if (cc_collecting) return;
    // a deinit body is user code running mid-cascade: its allocations must
    // not start a collection — a mid-destroy object must never be walked.
    // cc_pending stays set, so the next allocation after the cascade retries.
    if (__atomic_load_n(&beans_in_deinit, __ATOMIC_RELAXED)) return;
    ARC_ADD(arc_collections, 1);
    cc_collecting = 1;
    if (cc_is_mt()) pthread_mutex_lock(&cc_mu);

    // keep only live purple candidates; zombies (released while parked)
    // just need their shells freed, everything else drops out
    long long n = 0;
    for (long long i = 0; i < cc_len; i++) {
        void* p = cc_roots[i];
        BHead* h = head_of(p);
        if (cc_color(h) == CC_PURPLE && RC_COUNT(h->rc) > 0) {
            cc_roots[n++] = p;
        } else {
            __atomic_and_fetch(&h->meta, ~CC_BUF, __ATOMIC_RELAXED);
            if (RC_COUNT(h->rc) == 0) {
                void* child = cc_free_shell(p, h->meta);
                if (child) beans_release(child);
            }
        }
    }
    cc_len = n;

    // The filter above is the cheap half and just ran: husk shells free at
    // a steady cadence, so they can never pile past survivors + 256. Trial
    // deletion is the expensive half — it walks everything reachable from
    // the survivors — so it only runs once enough purple candidates pile
    // up, and it backs off hard when a walk frees nothing: a live tree
    // that gets borrow-pinned on every visit must not be re-walked every
    // few hundred allocations (that made binary-trees 10x slower than Go).
    if (cc_len && (force || cc_len >= cc_walk_min)) {
        CCStack st = {0, 0, 0}, aux = {0, 0, 0}, dead = {0, 0, 0};
        for (long long i = 0; i < cc_len; i++) cc_mark_gray(cc_roots[i], &st);
        for (long long i = 0; i < cc_len; i++) cc_scan(cc_roots[i], &st, &aux);
        for (long long i = 0; i < cc_len; i++) {
            BHead* h = head_of(cc_roots[i]);
            __atomic_and_fetch(&h->meta, ~CC_BUF, __ATOMIC_RELAXED);
            cc_collect_white(cc_roots[i], &st, &dead);
        }
        cc_len = 0;
        ARC_ADD(arc_cycle_objects, dead.len);
        // nothing was freed while walking, so no stale pointer was ever
        // read; now the whole white set goes at once
        for (long long i = 0; i < dead.len; i++) {
            void* child = cc_free_shell(dead.v[i], head_of(dead.v[i])->meta);
            if (child) beans_release(child);
        }
        cc_walk_min = dead.len ? 256
                               : (cc_walk_min * 4 > (1LL << 18) ? (1LL << 18)
                                                                : cc_walk_min * 4);
        free(st.v);
        free(aux.v);
        free(dead.v);
    }
    // geometric re-arm: survivors stay parked, so the next filter may scan
    // them again — amortized O(1) per park only if the buffer must grow by
    // its own size first. Husk shells thus wait at most 2·survivors + 256
    // parks, which keeps RSS flat in practice (husk-heavy programs have few
    // long-lived survivors)
    cc_threshold = cc_len * 2 + 256;
    cc_pending = 0;
    if (cc_is_mt()) pthread_mutex_unlock(&cc_mu);
    cc_collecting = 0;
}

static void cc_at_exit(void) {
    if (cc_threads == 0) cc_collect(1); // forced: leaks must see 0 at exit
}
__attribute__((constructor)) static void cc_setup(void) { atexit(cc_at_exit); }

void beans_panic(const char* msg, long long line, long long col) {
    fflush(stdout); // ordered output: buffered stdout before the stderr panic
    fprintf(stderr, "runtime panic at %lld:%lld: %s\n", line, col, msg);
    exit(3);
}
// list bounds — message matches the interpreter's, index and length included
void beans_panic_index(long long i, long long len, long long has_len,
                       long long line, long long col) {
    char b[96];
    if (has_len) snprintf(b, sizeof b, "list index %lld out of range (len %lld)", i, len);
    else snprintf(b, sizeof b, "list index %lld out of range", i);
    beans_panic(b, line, col);
}
void beans_panic_array_index(long long i, long long len,
                             long long line, long long col) {
    char b[96];
    snprintf(b, sizeof b, "array index %lld out of range (len %lld)", i, len);
    beans_panic(b, line, col);
}
void beans_panic_slice_index(long long i, long long len,
                             long long line, long long col) {
    char b[96];
    snprintf(b, sizeof b, "slice index %lld out of range (len %lld)", i, len);
    beans_panic(b, line, col);
}

// ---- strings (leaf allocations) ----
// a string's byte length lives in its meta shape bits (kind 0 uses none of
// bits 3-60), so len is O(1) and never strlen. Read through beans_slen —
// masking with CC_SHAPE is mandatory, colors share the word.
static long long beans_slen(char* s) { return (head_of(s)->meta & CC_SHAPE) >> 3; }
static char* str_make(const char* p, long long n);
static char* rc_strdup(const char* s) {
    size_t n = strlen(s);
    char* r = beans_alloc((long long)n + 1, (long long)n << 3);
    memcpy(r, s, n + 1);
    return r;
}
// hand-rolled digits: snprintf("%lld") was ~1/3 of the string-build loop in
// the strings bench (vfprintf machinery per call); this matches its output
// byte for byte
static long long uint_digits(unsigned long long v) {
    long long n = 1;
    while (v >= 10) { v /= 10; n += 1; }
    return n;
}
static char* write_uint(char* out, unsigned long long v) {
    long long n = uint_digits(v);
    char* end = out + n;
    char* p = end;
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return end;
}
static long long int_digits(long long v) {
    unsigned long long u =
        v < 0 ? (unsigned long long)-(v + 1) + 1 : (unsigned long long)v;
    return uint_digits(u) + (v < 0);
}
static char* write_int(char* out, long long v) {
    unsigned long long u =
        v < 0 ? (unsigned long long)-(v + 1) + 1 : (unsigned long long)v;
    if (v < 0) *out++ = '-';
    return write_uint(out, u);
}
char* beans_from_int(long long v) {
    char b[24];
    char* e = b + sizeof b;
    char* p = e;
    unsigned long long u =
        v < 0 ? (unsigned long long)-(v + 1) + 1 : (unsigned long long)v;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    return str_make(p, e - p);
}
char* beans_from_uint(unsigned long long v) {
    char b[24];
    char* e = b + sizeof b;
    char* p = e;
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return str_make(p, e - p);
}
char* beans_from_float(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.10g", v);
    return rc_strdup(b);
}
char* beans_from_bool(int v) { return rc_strdup(v ? "true" : "false"); }
char* beans_concat(char* a, char* b) {
    size_t la = (size_t)beans_slen(a), lb = (size_t)beans_slen(b);
    char* r = beans_alloc((long long)(la + lb + 1), (long long)(la + lb) << 3);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}
char* beans_interpolate(long long n, ...) {
    va_list ap;
    va_start(ap, n);
    long long total = 0;
    for (long long i = 0; i < n; i++) {
        long long kind = va_arg(ap, long long);
        if (kind == 0) {
            total += beans_slen(va_arg(ap, char*));
        } else if (kind == 1) {
            total += int_digits(va_arg(ap, long long));
        } else if (kind == 2) {
            total += uint_digits(va_arg(ap, unsigned long long));
        } else if (kind == 3) {
            char b[48];
            total += snprintf(b, sizeof b, "%.10g", va_arg(ap, double));
        } else {
            total += va_arg(ap, int) ? 4 : 5;
        }
    }
    va_end(ap);
    char* r = beans_alloc(total + 1, total << 3);
    char* w = r;
    va_start(ap, n);
    for (long long i = 0; i < n; i++) {
        long long kind = va_arg(ap, long long);
        if (kind == 0) {
            char* part = va_arg(ap, char*);
            long long len = beans_slen(part);
            memcpy(w, part, (size_t)len);
            w += len;
        } else if (kind == 1) {
            w = write_int(w, va_arg(ap, long long));
        } else if (kind == 2) {
            w = write_uint(w, va_arg(ap, unsigned long long));
        } else if (kind == 3) {
            char b[48];
            int len = snprintf(b, sizeof b, "%.10g", va_arg(ap, double));
            memcpy(w, b, (size_t)len);
            w += len;
        } else {
            int value = va_arg(ap, int);
            const char* text = value ? "true" : "false";
            long long len = value ? 4 : 5;
            memcpy(w, text, (size_t)len);
            w += len;
        }
    }
    va_end(ap);
    return r;
}
// strings carry their byte length and may legally hold NUL (\0 escapes,
// File.read) — every consumer here is length-based; C-string fns like fputs,
// strcmp, and strstr would silently stop at the first NUL and diverge from
// the interpreter
static char* str_make(const char* p, long long n);
void beans_println(char* s) {
    fwrite(s, 1, (size_t)beans_slen(s), stdout);
    fputc('\n', stdout);
}
void beans_print(char* s) { fwrite(s, 1, (size_t)beans_slen(s), stdout); }
// std::string semantics: bytes compare unsigned over the shorter length,
// ties break on length
int beans_str_cmp(char* a, char* b) {
    long long la = beans_slen(a), lb = beans_slen(b);
    long long n = la < lb ? la : lb;
    int c = n ? memcmp(a, b, (size_t)n) : 0;
    if (c) return c;
    return la < lb ? -1 : la > lb ? 1 : 0;
}
long long beans_str_len(char* s) { return beans_slen(s); }
char* beans_str_last(char* s, long long n) {
    long long len = beans_slen(s);
    if (n < 0) n = 0;
    if (n > len) n = len;
    return str_make(s + (len - n), n);
}
// leftmost match: memchr for the first byte (SIMD in libc), memcmp for the
// tail. memcmp-at-every-offset made contains/replace/split the hot spot of
// the strings bench, 2x behind Go's bytealg search.
static long long str_search(const char* s, long long n, const char* sub,
                            long long m, long long from) {
    if (m > n - from) return -1;
    if (m == 0) return from;
    const char* end = s + n;
    const char* p = s + from;
    for (;;) {
        long long room = (end - p) - (m - 1);
        if (room <= 0) return -1;
        const char* hit = memchr(p, sub[0], (size_t)room);
        if (!hit) return -1;
        if (m == 1 || memcmp(hit + 1, sub + 1, (size_t)(m - 1)) == 0) {
            return hit - s;
        }
        p = hit + 1;
    }
}
long long beans_str_contains(char* s, char* sub) {
    long long n = beans_slen(s), m = beans_slen(sub);
    if (m == 0) return 1;
    return str_search(s, n, sub, m, 0) >= 0;
}

// a ready-made Error box: [vtable null][-1][msg][kind] — the exact 32-byte
// layout codegen's make_error builds; meta 97 marks slots 2 and 3 as pointers
static void* mk_error(const char* msg, const char* kind) {
    long long* e = beans_alloc(32, 97);
    e[1] = -1;
    e[2] = (long long)rc_strdup(msg);
    e[3] = (long long)rc_strdup(kind);
    return e;
}
// like mk_error, but msg is already an rc string carrying its exact byte
// length — user text can hold NUL and must not pass through strlen
static void* mk_error_own(char* msg_rc, const char* kind) {
    long long* e = beans_alloc(32, 97);
    e[1] = -1;
    e[2] = (long long)msg_rc;
    e[3] = (long long)rc_strdup(kind);
    return e;
}

// fallible-builtin ABI: 16 bytes so C and IR both return it in registers.
// err null = ok(val); err set = a ready Error object the caller boxes.
typedef struct {
    long long val;
    void* err;
} BRes;

static BRes parse_fail(const char* s, const char* what) {
    // s is the beans receiver string — splice it by its stored length so an
    // embedded NUL keeps the message byte-identical to the interpreter's
    const char* p1 = "can't read '";
    const char* p2 = "' as ";
    long long ls = beans_slen((char*)s);
    size_t l1 = strlen(p1), l2 = strlen(p2), lw = strlen(what);
    long long total = (long long)l1 + ls + (long long)l2 + (long long)lw;
    char* m = beans_alloc(total + 1, total << 3);
    char* w = m;
    memcpy(w, p1, l1);
    w += l1;
    memcpy(w, s, (size_t)ls);
    w += ls;
    memcpy(w, p2, l2);
    w += l2;
    memcpy(w, what, lw);
    return (BRes){0, mk_error_own(m, "")};
}

BRes beans_str_to_int(char* s) {
    char* end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') return parse_fail(s, "int");
    return (BRes){v, NULL};
}

BRes beans_str_to_float(char* s) {
    char* end = NULL;
    double d = strtod(s, &end);
    if (end == s || *end != '\0') return parse_fail(s, "float");
    BRes r;
    r.err = NULL;
    memcpy(&r.val, &d, 8);
    return r;
}

// Option-shaped ABI: has 0 = none
typedef struct {
    long long val;
    long long has;
} BOpt;

// explicit-length string maker; the terminator byte is already zero
// because every allocation path hands back zeroed memory
static char* str_make(const char* p, long long n) {
    char* r = beans_alloc(n + 1, n << 3);
    memcpy(r, p, (size_t)n);
    return r;
}

long long beans_str_is_empty(char* s) { return beans_slen(s) == 0; }
char* beans_str_first(char* s, long long n) {
    long long len = beans_slen(s);
    if (n < 0) n = 0;
    if (n > len) n = len;
    return str_make(s, n);
}
long long beans_str_starts_with(char* s, char* p) {
    long long pl = beans_slen(p);
    return pl <= beans_slen(s) && memcmp(s, p, (size_t)pl) == 0;
}
long long beans_str_ends_with(char* s, char* p) {
    long long n = beans_slen(s), pl = beans_slen(p);
    return pl <= n && memcmp(s + n - pl, p, (size_t)pl) == 0;
}
// empty needle: find says 0, rfind says len — the C++ side agrees
BOpt beans_str_find(char* s, char* sub) {
    long long n = beans_slen(s), m = beans_slen(sub);
    if (m == 0) return (BOpt){0, 1};
    long long i = str_search(s, n, sub, m, 0);
    if (i >= 0) return (BOpt){i, 1};
    return (BOpt){0, 0};
}
BOpt beans_str_rfind(char* s, char* sub) {
    long long n = beans_slen(s), m = beans_slen(sub);
    if (m == 0) return (BOpt){n, 1};
    for (long long i = n - m; i >= 0; i--) {
        if (memcmp(s + i, sub, (size_t)m) == 0) return (BOpt){i, 1};
    }
    return (BOpt){0, 0};
}
char* beans_str_slice(char* s, long long from, long long to, long long line,
                      long long col) {
    long long n = beans_slen(s);
    if (from < 0 || to < from || to > n) {
        char m[96];
        snprintf(m, sizeof m, "slice %lld..%lld out of range (len %lld)", from, to, n);
        beans_panic(m, line, col);
    }
    return str_make(s + from, to - from);
}
long long beans_str_byte_at(char* s, long long i, long long line, long long col) {
    long long n = beans_slen(s);
    if (i < 0 || i >= n) {
        char m[80];
        snprintf(m, sizeof m, "byte index %lld out of range (len %lld)", i, n);
        beans_panic(m, line, col);
    }
    return (long long)(unsigned char)s[i];
}
static int str_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
char* beans_str_trim(char* s) {
    long long b = 0, e = beans_slen(s);
    while (b < e && str_is_ws(s[b])) b++;
    while (e > b && str_is_ws(s[e - 1])) e--;
    return str_make(s + b, e - b);
}
char* beans_str_trim_start(char* s) {
    long long b = 0, e = beans_slen(s);
    while (b < e && str_is_ws(s[b])) b++;
    return str_make(s + b, e - b);
}
char* beans_str_trim_end(char* s) {
    long long e = beans_slen(s);
    while (e > 0 && str_is_ws(s[e - 1])) e--;
    return str_make(s, e);
}
char* beans_str_to_upper(char* s) {
    long long n = beans_slen(s);
    char* r = str_make(s, n);
    for (long long i = 0; i < n; i++) {
        if (r[i] >= 'a' && r[i] <= 'z') r[i] = (char)(r[i] - 'a' + 'A');
    }
    return r;
}
char* beans_str_to_lower(char* s) {
    long long n = beans_slen(s);
    char* r = str_make(s, n);
    for (long long i = 0; i < n; i++) {
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    }
    return r;
}
char* beans_str_replace(char* s, char* old, char* nw) {
    long long n = beans_slen(s), m = beans_slen(old), rl = beans_slen(nw);
    if (m == 0) return str_make(s, n); // replacing nothing changes nothing
    long long count = 0;
    for (long long i = str_search(s, n, old, m, 0); i >= 0;
         i = str_search(s, n, old, m, i + m)) {
        count++;
    }
    if (count == 0) return str_make(s, n);
    long long outn = n + count * (rl - m);
    char* out = beans_alloc(outn + 1, outn << 3);
    char* w = out;
    long long i = 0;
    for (;;) {
        long long j = str_search(s, n, old, m, i);
        if (j < 0) break;
        memcpy(w, s + i, (size_t)(j - i));
        w += j - i;
        memcpy(w, nw, (size_t)rl);
        w += rl;
        i = j + m;
    }
    memcpy(w, s + i, (size_t)(n - i));
    return out;
}
char* beans_str_repeat(char* s, long long n, long long line, long long col) {
    if (n < 0) {
        char m[64];
        snprintf(m, sizeof m, "negative repeat count %lld", n);
        beans_panic(m, line, col);
    }
    long long len = beans_slen(s);
    long long outn = len * n;
    char* out = beans_alloc(outn + 1, outn << 3);
    for (long long i = 0; i < n; i++) memcpy(out + i * len, s, (size_t)len);
    return out;
}
long long beans_f64_round(double v) { return llround(v); }

// ---- lists ----
BList* beans_list_new(long long elem_ptr) {
    BList* l = beans_alloc(sizeof(BList), 2 | (elem_ptr << 3));
    l->cap = 4;
    l->data = calloc(4, 8);
    return l;
}
void beans_list_push(BList* l, long long v) {
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, (size_t)l->cap * 8);
    }
    l->data[l->len++] = v;
}
void beans_list_reserve(BList* l, long long capacity, long long line, long long col) {
    if (capacity < 0) {
        char b[64];
        snprintf(b, sizeof b, "negative reserve capacity %lld", capacity);
        beans_panic(b, line, col);
    }
    if (capacity > (1LL << 58)) beans_panic("reserve capacity too large", line, col);
    if (capacity <= l->cap) return;
    long long cap = l->cap;
    while (cap < capacity && cap <= (1LL << 60)) cap *= 2;
    if (cap < capacity) cap = capacity;
    l->data = realloc(l->data, (size_t)cap * 8);
    if (!l->data) beans_panic("out of memory", line, col);
    l->cap = cap;
}

// ---- class hierarchy (table emitted by the compiler) ----
extern long long beans_class_parents[];
long long beans_is_a(long long id, long long target) {
    while (id >= 0) {
        if (id == target) return 1;
        id = beans_class_parents[id];
    }
    return 0;
}

// ---- list search helpers (kind: 0 int-ish, 1 f64, 2 string, 3 decimal,
// 4 unordered — everything compares equal, so sort keeps the original order
// and min/max return the first element, like the interpreter's value_less
// returning false) ----
struct BDec;
int beans_dec_cmp(struct BDec* a, struct BDec* b);
static int slot_cmp(long long a, long long b, long long kind) {
    if (kind == 1) {
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind == 2) return beans_str_cmp((char*)a, (char*)b);
    if (kind == 3) return beans_dec_cmp((struct BDec*)a, (struct BDec*)b);
    if (kind == 4) return 0;
    if (kind == 5) {
        unsigned long long x = (unsigned long long)a;
        unsigned long long y = (unsigned long long)b;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind == 6) {
        unsigned aa = (unsigned)a, bb = (unsigned)b;
        float x, y;
        memcpy(&x, &aa, 4);
        memcpy(&y, &bb, 4);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    return a < b ? -1 : a > b ? 1 : 0;
}
// content equality for strings — length header first, bytes second; strcmp
// would stop at an embedded NUL and lie
long long beans_str_eq(char* a, char* b) {
    long long n = beans_slen(a);
    return n == beans_slen(b) && memcmp(a, b, (size_t)n) == 0;
}
// equality kinds (separate lattice from the ordering kinds above), matching
// the interpreter's value_eq arm for arm: 0 raw slot (ints, bools, pointer
// identity), 1 f64 by IEEE value (NaN equals nothing), 2 string content,
// 3 decimal value, 4 caller-supplied structural eq (enums, Bytes), 5 never
// equal (maps and resource handles — value_eq's default arm)
static long long slot_eq(long long a, long long b, long long kind,
                         long long (*eq)(long long, long long)) {
    if (kind == 0) return a == b;
    if (kind == 1) {
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return x == y;
    }
    if (kind == 2) return beans_str_eq((char*)a, (char*)b);
    if (kind == 3) return beans_dec_cmp((struct BDec*)a, (struct BDec*)b) == 0;
    if (kind == 4) return eq(a, b) != 0;
    if (kind == 6) {
        unsigned aa = (unsigned)a, bb = (unsigned)b;
        float x, y;
        memcpy(&x, &aa, 4);
        memcpy(&y, &bb, 4);
        return x == y;
    }
    return 0;
}
// hashes for the map index, one per equality kind. The contract is only that
// slot_eq-equal keys hash equal; the interpreter hashes differently and that
// is fine — nothing observable depends on hash values, iteration walks data.
static unsigned long long beans_mix64(unsigned long long x) {
    // One multiply is enough for an in-process table: unlike a persisted
    // cryptographic hash, this only needs to spread sequential integers and
    // aligned pointers across a power-of-two index. The old Murmur finalizer
    // used two multiplies and six dependent operations on every lookup.
    x ^= x >> 32;
    x *= 0xd6e8feb86659fd93ULL;
    x ^= x >> 32;
    return x;
}
long long beans_slot_mix(long long v) { return (long long)beans_mix64((unsigned long long)v); }
long long beans_f64_hash(long long v) {
    double x;
    memcpy(&x, &v, 8);
    if (x == 0.0) return (long long)beans_mix64(0); // -0.0 == 0.0
    return (long long)beans_mix64((unsigned long long)v);
}
long long beans_f32_hash(long long v) {
    unsigned bits = (unsigned)v;
    float x;
    memcpy(&x, &bits, 4);
    if (x == 0.0f) return (long long)beans_mix64(0);
    return (long long)beans_mix64(bits);
}
long long beans_str_hash(char* s) {
    long long n = beans_slen(s);
    unsigned long long h = 1469598103934665603ULL;
    for (long long i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return (long long)beans_mix64(h);
}
long long beans_dec_hash(struct BDec* d);
long long beans_bytes_hash(BList* b) {
    unsigned long long h = 1469598103934665603ULL;
    for (long long i = 0; i < b->len; i++) {
        h ^= ((unsigned char*)b->data)[i];
        h *= 1099511628211ULL;
    }
    return (long long)beans_mix64(h);
}
static unsigned long long slot_hash(long long v, long long kind,
                                    long long (*hf)(long long)) {
    if (kind == 1) return (unsigned long long)beans_f64_hash(v);
    if (kind == 2) return (unsigned long long)beans_str_hash((char*)v);
    if (kind == 3) return (unsigned long long)beans_dec_hash((struct BDec*)v);
    if (kind == 4) return (unsigned long long)hf(v);
    if (kind == 6) return (unsigned long long)beans_f32_hash(v);
    return beans_mix64((unsigned long long)v); // raw, and never-equal keys
}
long long beans_list_max(BList* l, long long kind, long long* ok) {
    *ok = l->len > 0;
    if (!*ok) return 0;
    long long best = l->data[0];
    for (long long i = 1; i < l->len; i++) {
        if (slot_cmp(l->data[i], best, kind) > 0) best = l->data[i];
    }
    return best;
}
long long beans_list_contains(BList* l, long long v, long long kind, void* eq) {
    for (long long i = 0; i < l->len; i++) {
        if (slot_eq(l->data[i], v, kind, (long long (*)(long long, long long))eq)) return 1;
    }
    return 0;
}
long long beans_list_min(BList* l, long long kind, long long* ok) {
    *ok = l->len > 0;
    if (!*ok) return 0;
    long long best = l->data[0];
    for (long long i = 1; i < l->len; i++) {
        if (slot_cmp(l->data[i], best, kind) < 0) best = l->data[i];
    }
    return best;
}
long long beans_list_index(BList* l, long long v, long long kind, long long* ok,
                           void* eq) {
    for (long long i = 0; i < l->len; i++) {
        if (slot_eq(l->data[i], v, kind, (long long (*)(long long, long long))eq)) {
            *ok = 1;
            return i;
        }
    }
    *ok = 0;
    return 0;
}
void beans_list_insert(BList* l, long long i, long long v, long long line,
                       long long col) {
    if (i < 0 || i > l->len) {
        char b[96];
        snprintf(b, sizeof b, "insert at %lld out of range (len %lld)", i, l->len);
        beans_panic(b, line, col);
    }
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, (size_t)l->cap * 8);
    }
    memmove(l->data + i + 1, l->data + i, (size_t)(l->len - i) * 8);
    l->data[i] = v;
    l->len += 1;
}
long long beans_list_remove(BList* l, long long i, long long line, long long col) {
    if (i < 0 || i >= l->len) {
        char b[96];
        snprintf(b, sizeof b, "list index %lld out of range (len %lld)", i, l->len);
        beans_panic(b, line, col);
    }
    long long v = l->data[i];
    memmove(l->data + i, l->data + i + 1, (size_t)(l->len - i - 1) * 8);
    l->len -= 1;
    return v; // the caller now owns the moved-out ref
}
void beans_list_reverse(BList* l) {
    for (long long i = 0, j = l->len - 1; i < j; i++, j--) {
        long long t = l->data[i];
        l->data[i] = l->data[j];
        l->data[j] = t;
    }
}
void beans_list_clear(BList* l) {
    // last element first — deinit made death order observable, and the
    // interpreter's vector teardown destroys back to front
    if ((head_of(l)->meta & CC_SHAPE) >> 3 & 1) {
        for (long long i = l->len; i-- > 0;) {
            if (l->data[i]) beans_release((void*)l->data[i]);
        }
    }
    l->len = 0;
}
BList* beans_list_slice(BList* l, long long from, long long to, long long line,
                        long long col) {
    if (from < 0 || to < from || to > l->len) {
        char b[96];
        snprintf(b, sizeof b, "slice %lld..%lld out of range (len %lld)", from, to,
                 l->len);
        beans_panic(b, line, col);
    }
    long long rc = (head_of(l)->meta & CC_SHAPE) >> 3 & 1;
    long long n = to - from;
    BList* r = beans_alloc(sizeof(BList), 2 | (rc << 3));
    r->len = n;
    r->cap = n > 4 ? n : 4;
    r->data = malloc((size_t)r->cap * sizeof(long long));
    if (!r->data) beans_panic("out of memory", line, col);
    memcpy(r->data, l->data + from, (size_t)n * sizeof(long long));
    if (rc) {
        for (long long i = 0; i < n; i++) {
            if (r->data[i]) beans_retain((void*)r->data[i]);
        }
    }
    return r;
}
BList* beans_list_clone(BList* l) {
    long long rc = (head_of(l)->meta & CC_SHAPE) >> 3 & 1;
    BList* r = beans_alloc(sizeof(BList), 2 | (rc << 3));
    r->len = l->len;
    r->cap = l->len > 4 ? l->len : 4;
    r->data = malloc((size_t)r->cap * sizeof(long long));
    if (!r->data) beans_panic("out of memory", 0, 0);
    memcpy(r->data, l->data, (size_t)l->len * sizeof(long long));
    if (rc) {
        for (long long i = 0; i < r->len; i++) {
            if (r->data[i]) beans_retain((void*)r->data[i]);
        }
    }
    return r;
}

// bottom-up stable merge — structurally identical to the interpreter's
// stable_merge, so both backends produce the same order for ANY predicate,
// even one that is not a strict weak ordering
static long long sort_less(long long x, long long y, long long kind, void* thunk,
                           void* box) {
    if (thunk) return ((long long (*)(void*, long long, long long))thunk)(box, x, y);
    return slot_cmp(x, y, kind) < 0;
}
static void list_merge_sort(long long* a, long long n, long long kind, void* thunk,
                            void* box) {
    if (n < 2) return;
    long long* buf = malloc((size_t)n * 8);
    for (long long w = 1; w < n; w *= 2) {
        for (long long lo = 0; lo < n; lo += 2 * w) {
            long long mid = lo + w < n ? lo + w : n;
            long long hi = lo + 2 * w < n ? lo + 2 * w : n;
            if (mid >= hi) continue;
            long long i = lo, j = mid, o = lo;
            while (i < mid && j < hi) {
                if (!sort_less(a[j], a[i], kind, thunk, box)) buf[o++] = a[i++];
                else buf[o++] = a[j++];
            }
            while (i < mid) buf[o++] = a[i++];
            while (j < hi) buf[o++] = a[j++];
            memcpy(a + lo, buf + lo, (size_t)(hi - lo) * 8);
        }
    }
    free(buf);
}
// Signed integer slots have a much cheaper stable path: four 16-bit passes,
// with the sign bit flipped for normal two's-complement ordering.
// This avoids a comparator branch for every merge comparison and keeps equal
// values in input order. bool uses the same path (its slots are 0/1).
static void list_radix_sort_int(long long* a, long long n) {
    if (n < 2) return;
    long long* buf = malloc((size_t)n * 8);
    long long* src = a;
    long long* dst = buf;
    size_t* count = malloc(65536 * sizeof(size_t));
    size_t* at = malloc(65536 * sizeof(size_t));
    for (int pass = 0; pass < 4; pass++) {
        memset(count, 0, 65536 * sizeof(size_t));
        int shift = pass * 16;
        for (long long i = 0; i < n; i++) {
            unsigned long long key = (unsigned long long)src[i] ^ (1ULL << 63);
            count[(key >> shift) & 65535]++;
        }
        size_t sum = 0;
        for (int word = 0; word < 65536; word++) {
            at[word] = sum;
            sum += count[word];
        }
        for (long long i = 0; i < n; i++) {
            unsigned long long key = (unsigned long long)src[i] ^ (1ULL << 63);
            dst[at[(key >> shift) & 65535]++] = src[i];
        }
        long long* swap = src;
        src = dst;
        dst = swap;
    }
    // Four passes land back in `a`; keep this copy for defensive use if the
    // pass count changes later.
    if (src != a) memcpy(a, src, (size_t)n * 8);
    free(count);
    free(at);
    free(buf);
}
void beans_list_sort(BList* l, long long kind) {
    if (kind == 0) list_radix_sort_int(l->data, l->len);
    else list_merge_sort(l->data, l->len, kind, NULL, NULL);
}
void beans_list_sort_by(BList* l, void* thunk, void* box) {
    list_merge_sort(l->data, l->len, 0, thunk, box);
}
void beans_list_sort_by_key(BList* l, void* thunk, void* box) {
    long long n = l->len;
    if (n < 2) return;
    long long* keys = malloc((size_t)n * 8);
    long long* key_buf = malloc((size_t)n * 8);
    long long* val_buf = malloc((size_t)n * 8);
    size_t* count = malloc(65536 * sizeof(size_t));
    size_t* at = malloc(65536 * sizeof(size_t));
    long long (*key_fn)(void*, long long) =
        (long long (*)(void*, long long))thunk;
    for (long long i = 0; i < n; i++) keys[i] = key_fn(box, l->data[i]);
    long long* key_src = keys;
    long long* key_dst = key_buf;
    long long* val_src = l->data;
    long long* val_dst = val_buf;
    for (int pass = 0; pass < 4; pass++) {
        memset(count, 0, 65536 * sizeof(size_t));
        int shift = pass * 16;
        for (long long i = 0; i < n; i++) {
            unsigned long long key = (unsigned long long)key_src[i] ^ (1ULL << 63);
            count[(key >> shift) & 65535]++;
        }
        size_t sum = 0;
        for (int word = 0; word < 65536; word++) {
            at[word] = sum;
            sum += count[word];
        }
        for (long long i = 0; i < n; i++) {
            unsigned long long key = (unsigned long long)key_src[i] ^ (1ULL << 63);
            size_t out = at[(key >> shift) & 65535]++;
            key_dst[out] = key_src[i];
            val_dst[out] = val_src[i];
        }
        long long* swap = key_src; key_src = key_dst; key_dst = swap;
        swap = val_src; val_src = val_dst; val_dst = swap;
    }
    if (val_src != l->data) memcpy(l->data, val_src, (size_t)n * 8);
    free(keys);
    free(key_buf);
    free(val_buf);
    free(count);
    free(at);
}

// ---- maps ----
// A flat key/value array plus an open-addressed index. OrderedMap leaves stable
// removal holes. Map swap-removes. Small maps have no index and scan linearly.
#define MAP_LINEAR_MAX 8
#define IDX_POS 0xffffffffULL /* low 32: pos+2, 1 = tombstone */
#define IDX_FRAG 0xffffffff00000000ULL /* high 32: hash fragment */
#define MAP_DEAD(m, p) ((m)->deadbits && (m)->deadbits[(p) >> 6] >> ((p)&63) & 1)
BMap* beans_map_new(long long key_ptr, long long val_ptr, long long ordered) {
    BMap* m = beans_alloc(sizeof(BMap), 3 | (key_ptr << 3) | (val_ptr << 4));
    m->cap = 4;
    m->data = calloc(8, 8); // idx/tombs/used/deadbits start zero: beans_alloc zeroes
    m->ordered = ordered;
    return m;
}
// (re)build the index sized for the current entry count, dropping tombstones
// and compacting holes. Only moves slots and writes index words — never
// retains or releases.
static void map_reindex_to(BMap* m, long long kind, long long (*hf)(long long),
                           long long reserve) {
    if (m->deadbits) {
        long long w = 0;
        for (long long p = 0; p < m->used; p++) {
            if (MAP_DEAD(m, p)) continue;
            if (w != p) {
                m->data[w * 2] = m->data[p * 2];
                m->data[w * 2 + 1] = m->data[p * 2 + 1];
            }
            w += 1;
        }
        free(m->deadbits);
        m->deadbits = NULL;
        m->used = w; // == m->len
    }
    long long wanted = m->len > reserve ? m->len : reserve;
    long long cap = 16;
    while (wanted * 3 >= cap * 2) cap <<= 1;
    free(m->idx);
    m->idx = calloc((size_t)cap, 8);
    m->icap = cap;
    m->tombs = 0;
    unsigned long long mask = (unsigned long long)cap - 1;
    for (long long p = 0; p < m->len; p++) {
        unsigned long long h = slot_hash(m->data[p * 2], kind, hf);
        unsigned long long i = h & mask;
        while (m->idx[i] & IDX_POS) i = (i + 1) & mask;
        m->idx[i] = (h & IDX_FRAG) | (unsigned long long)(p + 2);
    }
}
static void map_reindex(BMap* m, long long kind, long long (*hf)(long long)) {
    map_reindex_to(m, kind, hf, 0);
}
void beans_map_reserve(BMap* m, long long capacity, long long kind, void* hash,
                       long long line, long long col) {
    if (capacity < 0) {
        char b[64];
        snprintf(b, sizeof b, "negative reserve capacity %lld", capacity);
        beans_panic(b, line, col);
    }
    if (capacity > (1LL << 58)) beans_panic("reserve capacity too large", line, col);
    if (capacity > m->cap) {
        long long cap = m->cap;
        while (cap < capacity && cap <= (1LL << 59)) cap *= 2;
        if (cap < capacity) cap = capacity;
        m->data = realloc(m->data, (size_t)cap * 16);
        if (!m->data) beans_panic("out of memory", line, col);
        if (m->deadbits) {
            long long old_words = (m->cap + 63) >> 6;
            long long new_words = (cap + 63) >> 6;
            m->deadbits = realloc(m->deadbits, (size_t)new_words * 8);
            memset(m->deadbits + old_words, 0,
                   (size_t)(new_words - old_words) * 8);
        }
        m->cap = cap;
    }
    map_reindex_to(m, kind, (long long (*)(long long))hash, capacity);
}
// keys compare with the same equality lattice as list search (slot_eq): raw,
// f64 value, string content, decimal value, structural thunk, never-equal.
// *hout is filled iff the index is active, so set can reuse it; *slot_out
// (may be NULL) gets the hit's index slot so remove can tombstone it O(1).
static long long map_find(BMap* m, long long key, long long kind, void* eq,
                          long long (*hf)(long long), unsigned long long* hout,
                          unsigned long long* slot_out) {
    if (!m->idx) {
        for (long long i = 0; i < m->len; i++) {
            if (slot_eq(m->data[i * 2], key, kind,
                        (long long (*)(long long, long long))eq)) return i;
        }
        return -1;
    }
    unsigned long long h = slot_hash(key, kind, hf);
    *hout = h;
    unsigned long long mask = (unsigned long long)m->icap - 1;
    unsigned long long frag = h & IDX_FRAG;
    unsigned long long first_tomb = ~0ULL;
    for (unsigned long long i = h & mask;; i = (i + 1) & mask) {
        unsigned long long w = m->idx[i];
        unsigned long long st = w & IDX_POS;
        if (st == 0) {
            if (slot_out) *slot_out = first_tomb != ~0ULL ? first_tomb : i;
            return -1;
        }
        if (st == 1 && first_tomb == ~0ULL) first_tomb = i;
        if (st >= 2 && (w & IDX_FRAG) == frag) {
            long long p = (long long)st - 2;
            if (slot_eq(m->data[p * 2], key, kind,
                        (long long (*)(long long, long long))eq)) {
                if (slot_out) *slot_out = i;
                return p;
            }
        }
    }
}
// Raw-slot keys (integers, bools, and identity keys) are the common map case.
// Keep their probe loop separate so every occupied bucket does not branch
// through the full equality/hash kind lattice.
static long long map_find_raw(BMap* m, long long key, unsigned long long* hout,
                              unsigned long long* slot_out) {
    if (!m->idx) {
        for (long long i = 0; i < m->len; i++) {
            if (m->data[i * 2] == key) return i;
        }
        return -1;
    }
    unsigned long long h = beans_mix64((unsigned long long)key);
    *hout = h;
    unsigned long long mask = (unsigned long long)m->icap - 1;
    unsigned long long frag = h & IDX_FRAG;
    unsigned long long first_tomb = ~0ULL;
    for (unsigned long long i = h & mask;; i = (i + 1) & mask) {
        unsigned long long w = m->idx[i];
        unsigned long long st = w & IDX_POS;
        if (st == 0) {
            if (slot_out) *slot_out = first_tomb != ~0ULL ? first_tomb : i;
            return -1;
        }
        if (st == 1 && first_tomb == ~0ULL) first_tomb = i;
        if (st >= 2 && (w & IDX_FRAG) == frag) {
            long long p = (long long)st - 2;
            if (m->data[p * 2] == key) {
                if (slot_out) *slot_out = i;
                return p;
            }
        }
    }
}

static void map_insert_miss(BMap* m, long long key, long long val,
                            unsigned long long h, long long kind,
                            long long (*hf)(long long),
                            unsigned long long insert_slot) {
    if (m->used == m->cap) {
        long long ow = (m->cap + 63) >> 6;
        m->cap *= 2;
        m->data = realloc(m->data, (size_t)m->cap * 16);
        if (m->deadbits) {
            long long nw = (m->cap + 63) >> 6;
            m->deadbits = realloc(m->deadbits, (size_t)nw * 8);
            memset(m->deadbits + ow, 0, (size_t)(nw - ow) * 8);
        }
    }
    m->data[m->used * 2] = key;
    m->data[m->used * 2 + 1] = val;
    m->used += 1;
    m->len += 1;
    if (!m->idx) {
        if (m->len > MAP_LINEAR_MAX) map_reindex(m, kind, hf);
    } else if ((m->used + m->tombs) * 3 >= m->icap * 2) {
        map_reindex(m, kind, hf);
    } else { // the miss probe already found the insertion slot
        if ((m->idx[insert_slot] & IDX_POS) == 1) m->tombs -= 1;
        m->idx[insert_slot] =
            (h & IDX_FRAG) | (unsigned long long)(m->used + 1);
    }
}

// note: the map owns key and value refs; the caller retains before calling
void beans_map_set(BMap* m, long long key, long long val, long long kind, void* eq,
                   void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    long long i = map_find(m, key, kind, eq, hf, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key); // duplicate key not stored
        if (flags & 2) beans_release((void*)m->data[i * 2 + 1]);
        m->data[i * 2 + 1] = val;
        return;
    }
    map_insert_miss(m, key, val, h, kind, hf, slot);
}

void beans_map_set_raw(BMap* m, long long key, long long val) {
    unsigned long long h = 0, slot = 0;
    long long i = map_find_raw(m, key, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        if (flags & 2) beans_release((void*)m->data[i * 2 + 1]);
        m->data[i * 2 + 1] = val;
        return;
    }
    map_insert_miss(m, key, val, h, 0, NULL, slot);
}

long long beans_map_insert(BMap* m, long long key, long long val, long long kind,
                           void* eq, void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    if (map_find(m, key, kind, eq, hf, &h, &slot) >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        if (flags & 2) beans_release((void*)val);
        return 0;
    }
    map_insert_miss(m, key, val, h, kind, hf, slot);
    return 1;
}

long long beans_map_insert_raw(BMap* m, long long key, long long val) {
    unsigned long long h = 0, slot = 0;
    if (map_find_raw(m, key, &h, &slot) >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        if (flags & 2) beans_release((void*)val);
        return 0;
    }
    map_insert_miss(m, key, val, h, 0, NULL, slot);
    return 1;
}

BOpt beans_map_get_raw(BMap* m, long long key) {
    unsigned long long h = 0;
    long long i = map_find_raw(m, key, &h, NULL);
    return i >= 0 ? (BOpt){m->data[i * 2 + 1], 1} : (BOpt){0, 0};
}

long long beans_map_contains_raw(BMap* m, long long key) {
    unsigned long long h = 0;
    return map_find_raw(m, key, &h, NULL) >= 0;
}

long long beans_map_get(BMap* m, long long key, long long kind, long long* ok,
                        void* eq, void* hash) {
    unsigned long long h = 0;
    long long i = map_find(m, key, kind, eq, (long long (*)(long long))hash, &h, 0);
    *ok = i >= 0;
    return i >= 0 ? m->data[i * 2 + 1] : 0;
}
static long long map_remove_found(BMap* m, long long i,
                                  unsigned long long slot, long long kind,
                                  long long (*hf)(long long)) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    if ((flags & 1) && m->data[i * 2]) beans_release((void*)m->data[i * 2]);
    if ((flags & 2) && m->data[i * 2 + 1]) beans_release((void*)m->data[i * 2 + 1]);
    if (!m->idx) {
        if (m->ordered) {
            memmove(m->data + i * 2, m->data + (i + 1) * 2,
                    (size_t)(m->used - i - 1) * 16);
        } else if (i != m->used - 1) {
            m->data[i * 2] = m->data[(m->used - 1) * 2];
            m->data[i * 2 + 1] = m->data[(m->used - 1) * 2 + 1];
        }
        m->len -= 1;
        m->used -= 1;
        return 1;
    }
    if (!m->ordered) {
        long long last = m->used - 1;
        m->idx[slot] = 1;
        m->tombs += 1;
        if (i != last) {
            long long moved_key = m->data[last * 2];
            unsigned long long h = slot_hash(moved_key, kind, hf);
            unsigned long long mask = (unsigned long long)m->icap - 1;
            for (unsigned long long at = h & mask;; at = (at + 1) & mask) {
                unsigned long long state = m->idx[at] & IDX_POS;
                if (state == (unsigned long long)(last + 2)) {
                    m->idx[at] = (m->idx[at] & IDX_FRAG) |
                                 (unsigned long long)(i + 2);
                    break;
                }
            }
            m->data[i * 2] = m->data[last * 2];
            m->data[i * 2 + 1] = m->data[last * 2 + 1];
        }
        m->data[last * 2] = 0;
        m->data[last * 2 + 1] = 0;
        m->len -= 1;
        m->used -= 1;
        if (m->tombs > m->len) map_reindex(m, kind, hf);
        return 1;
    }
    // indexed: zero the pair into a hole — no entry moves, so no index
    // position needs fixing and delete is O(1). Reindex compacts once
    // holes outnumber live entries, so the cost is amortized.
    m->data[i * 2] = 0;
    m->data[i * 2 + 1] = 0;
    if (!m->deadbits) m->deadbits = calloc((size_t)((m->cap + 63) >> 6), 8);
    m->deadbits[i >> 6] |= 1ULL << (i & 63);
    m->len -= 1;
    m->idx[slot] = 1; // map_find landed on the hit's slot
    m->tombs += 1;
    if (m->used > m->len * 2) map_reindex(m, kind, hf);
    return 1;
}
long long beans_map_remove(BMap* m, long long key, long long kind, void* eq,
                           void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    long long i = map_find(m, key, kind, eq, hf, &h, &slot);
    return i < 0 ? 0 : map_remove_found(m, i, slot, kind, hf);
}
long long beans_map_remove_raw(BMap* m, long long key) {
    unsigned long long h = 0, slot = 0;
    long long i = map_find_raw(m, key, &h, &slot);
    return i < 0 ? 0 : map_remove_found(m, i, slot, 0, NULL);
}
BMap* beans_map_clone(BMap* m, long long kind, void* hash) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    BMap* copy = beans_map_new(flags & 1, (flags >> 1) & 1, m->ordered);
    if (m->len > copy->cap) {
        copy->cap = m->len;
        copy->data = realloc(copy->data, (size_t)copy->cap * 16);
        if (!copy->data) beans_panic("out of memory", 0, 0);
    }
    for (long long i = 0; i < m->used; i++) {
        if (MAP_DEAD(m, i)) continue;
        long long key = m->data[i * 2];
        long long value = m->data[i * 2 + 1];
        if ((flags & 1) && key) beans_retain((void*)key);
        if ((flags & 2) && value) beans_retain((void*)value);
        copy->data[copy->used * 2] = key;
        copy->data[copy->used * 2 + 1] = value;
        copy->used += 1;
        copy->len += 1;
    }
    if (copy->len > MAP_LINEAR_MAX)
        map_reindex(copy, kind, (long long (*)(long long))hash);
    return copy;
}
BList* beans_map_keys(BMap* m) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    BList* l = beans_list_new(flags & 1);
    for (long long i = 0; i < m->used; i++) {
        if (MAP_DEAD(m, i)) continue;
        long long k = m->data[i * 2];
        if ((flags & 1) && k) beans_retain((void*)k);
        beans_list_push(l, k);
    }
    return l;
}
BList* beans_map_values(BMap* m) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    BList* l = beans_list_new((flags >> 1) & 1);
    for (long long i = 0; i < m->used; i++) {
        if (MAP_DEAD(m, i)) continue;
        long long v = m->data[i * 2 + 1];
        if ((flags & 2) && v) beans_retain((void*)v);
        beans_list_push(l, v);
    }
    return l;
}
void beans_map_clear(BMap* m) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    // reverse, value before key: the interpreter's pair teardown runs members
    // last-first, entries back to front — observable once a deinit prints
    for (long long i = m->used; i-- > 0;) { // holes are zeroed: null-skip
        if ((flags & 2) && m->data[i * 2 + 1]) beans_release((void*)m->data[i * 2 + 1]);
        if ((flags & 1) && m->data[i * 2]) beans_release((void*)m->data[i * 2]);
    }
    m->len = 0;
    m->used = 0;
    free(m->deadbits);
    m->deadbits = NULL;
    free(m->idx);
    m->idx = NULL;
    m->icap = 0;
    m->tombs = 0;
}

// element rendering matches the interpreter's display(): the kind code says
// how each slot turns into text (0 int, 1 f64, 2 str, 3 dec, 4 bool)
char* beans_dec_str(struct BDec* a);
char* beans_list_join(BList* l, char* sep, long long kind) {
    long long sl = beans_slen(sep);
    char** parts = malloc((size_t)(l->len ? l->len : 1) * sizeof(char*));
    long long total = 0;
    for (long long i = 0; i < l->len; i++) {
        long long v = l->data[i];
        char* s;
        if (kind == 2) {
            s = (char*)v;
        } else if (kind == 0) {
            s = beans_from_int(v);
        } else if (kind == 1) {
            double d;
            memcpy(&d, &v, 8);
            s = beans_from_float(d);
        } else if (kind == 3) {
            s = beans_dec_str((struct BDec*)v);
        } else {
            s = beans_from_bool((int)v);
        }
        parts[i] = s;
        total += beans_slen(s);
        if (i) total += sl;
    }
    char* out = beans_alloc(total + 1, total << 3);
    char* w = out;
    for (long long i = 0; i < l->len; i++) {
        if (i) {
            memcpy(w, sep, (size_t)sl);
            w += sl;
        }
        long long n = beans_slen(parts[i]);
        memcpy(w, parts[i], (size_t)n);
        w += n;
        if (kind != 2) beans_release(parts[i]); // rendered copies are ours
    }
    free(parts);
    return out;
}

// UTF-8 sequences, one string per character; a malformed lead or truncated
// tail comes through one byte at a time — byte slicing, no validation
BList* beans_str_chars(char* s) {
    long long len = beans_slen(s);
    BList* l = beans_list_new(1);
    long long i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        long long n = c < 0x80          ? 1
                      : (c >> 5) == 0x6 ? 2
                      : (c >> 4) == 0xE ? 3
                      : (c >> 3) == 0x1E ? 4
                                         : 1;
        if (i + n > len) {
            n = 1;
        } else {
            for (long long k = 1; k < n; k++) {
                if (((unsigned char)s[i + k] >> 6) != 0x2) {
                    n = 1;
                    break;
                }
            }
        }
        beans_list_push(l, (long long)str_make(s + i, n));
        i += n;
    }
    return l;
}
long long beans_str_count_chars(char* s, long long from, long long to,
                                long long line, long long col) {
    long long len = beans_slen(s);
    if (from < 0 || to < from || to > len) {
        char m[112];
        snprintf(m, sizeof m, "character range %lld..%lld out of range (len %lld)",
                 from, to, len);
        beans_panic(m, line, col);
    }
    long long count = 0;
    for (long long i = from; i < to; count++) {
        unsigned char c = (unsigned char)s[i];
        long long n = c < 0x80           ? 1
                      : (c >> 5) == 0x6  ? 2
                      : (c >> 4) == 0xE  ? 3
                      : (c >> 3) == 0x1E ? 4
                                         : 1;
        if (i + n > to) {
            n = 1;
        } else {
            for (long long k = 1; k < n; k++) {
                if (((unsigned char)s[i + k] >> 6) != 0x2) {
                    n = 1;
                    break;
                }
            }
        }
        i += n;
    }
    return count;
}

// ---- display through per-type show fns (emitted by the compiler) ----
// show(slot) returns an owned string; we copy and release it per element
static char* show_join(BList* l, const char* sep, long long sl,
                       char* (*show)(long long), int brackets) {
    long long cap = 16, len = 0;
    char* buf = malloc((size_t)cap);
    if (brackets) buf[len++] = '[';
    for (long long i = 0; i < l->len; i++) {
        char* s = show(l->data[i]);
        long long n = beans_slen(s);
        long long need = len + n + sl + 2;
        if (need > cap) {
            while (cap < need) cap *= 2;
            buf = realloc(buf, (size_t)cap);
        }
        if (i) {
            memcpy(buf + len, sep, (size_t)sl);
            len += sl;
        }
        memcpy(buf + len, s, (size_t)n);
        len += n;
        beans_release(s);
    }
    if (brackets) buf[len++] = ']';
    char* out = str_make(buf, len);
    free(buf);
    return out;
}
// ---- iterative show driver ----
// printing recursed on data depth (a 400k-link enum chain smashed the C
// stack); generated @bstep fns append their own text and PUSH child work
// instead of calling each other, and this driver drains the stack. Text
// items are borrowed (interned constants or C literals); scalar steps
// append their temporary and release it.
typedef struct {
    void* fn; // step fn, or null for a text item
    long long v;
    const char* text;
    long long tlen;
} BShowItem;
typedef struct BShowCtx {
    BShowItem* items;
    long long len, cap;
    char* out;
    long long olen, ocap;
} BShowCtx;
static void show_out(BShowCtx* c, const char* p, long long n) {
    if (c->olen + n + 1 > c->ocap) {
        c->ocap = (c->olen + n + 1) * 2 + 16;
        c->out = realloc(c->out, (size_t)c->ocap);
    }
    memcpy(c->out + c->olen, p, (size_t)n);
    c->olen += n;
}
void beans_show_append(BShowCtx* c, char* s) { show_out(c, s, beans_slen(s)); }
static void show_push(BShowCtx* c, void* fn, long long v, const char* t, long long tn) {
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->items = realloc(c->items, (size_t)c->cap * sizeof(BShowItem));
    }
    BShowItem it = {fn, v, t, tn};
    c->items[c->len++] = it;
}
void beans_show_push_val(BShowCtx* c, void* fn, long long v) {
    show_push(c, fn, v, NULL, 0);
}
void beans_show_push_lit(BShowCtx* c, char* s) {
    show_push(c, NULL, 0, s, beans_slen(s));
}
void beans_show_list_iter(BShowCtx* c, BList* l, void* elem_step) {
    show_out(c, "[", 1);
    show_push(c, NULL, 0, "]", 1);
    for (long long i = l->len; i-- > 1;) {
        show_push(c, elem_step, l->data[i], NULL, 0);
        show_push(c, NULL, 0, ", ", 2);
    }
    if (l->len > 0) show_push(c, elem_step, l->data[0], NULL, 0);
}
char* beans_show_run(void* fn, long long v) {
    BShowCtx c = {0, 0, 0, 0, 0, 0};
    show_push(&c, fn, v, NULL, 0);
    while (c.len > 0) {
        BShowItem it = c.items[--c.len];
        if (it.fn) ((void (*)(BShowCtx*, long long))it.fn)(&c, it.v);
        else show_out(&c, it.text, it.tlen);
    }
    char* r = str_make(c.out ? c.out : "", c.olen);
    free(c.out);
    free(c.items);
    return r;
}

char* beans_show_list(BList* l, char* (*show)(long long)) {
    return show_join(l, ", ", 2, show, 1);
}
char* beans_list_join_show(BList* l, char* sep, char* (*show)(long long)) {
    return show_join(l, sep, beans_slen(sep), show, 0);
}

// ---- string ops that build lists ----
BList* beans_str_split(char* s, char* sep) {
    BList* l = beans_list_new(1);
    long long n = beans_slen(s), m = beans_slen(sep);
    if (m == 0) { // no separator: the whole string, one piece
        beans_list_push(l, (long long)str_make(s, n));
        return l;
    }
    long long i = 0;
    for (long long j = str_search(s, n, sep, m, 0); j >= 0;
         j = str_search(s, n, sep, m, i)) {
        beans_list_push(l, (long long)str_make(s + i, j - i));
        i = j + m;
    }
    beans_list_push(l, (long long)str_make(s + i, n - i));
    return l;
}
BList* beans_str_lines(char* s) {
    BList* l = beans_list_new(1);
    long long n = beans_slen(s), i = 0;
    for (long long j = 0; j < n; j++) {
        if (s[j] == '\n') {
            beans_list_push(l, (long long)str_make(s + i, j - i));
            i = j + 1;
        }
    }
    // a trailing newline doesn't make an empty final line
    if (i < n) beans_list_push(l, (long long)str_make(s + i, n - i));
    return l;
}

// ---- Bytes: kind 2 with no element pointers — data freed, never walked ----
static BList* bytes_mk(long long n) {
    BList* b = beans_alloc(sizeof(BList), 2);
    long long cap = n < 8 ? 8 : n;
    b->data = calloc((size_t)cap, 1);
    if (!b->data) beans_panic("out of memory", 0, 0);
    b->len = n;
    b->cap = cap;
    return b;
}
BList* beans_bytes_new(long long n, long long line, long long col) {
    if (n < 0) {
        char m[48];
        snprintf(m, sizeof m, "negative size %lld", n);
        beans_panic(m, line, col);
    }
    return bytes_mk(n);
}
BList* beans_bytes_from(char* s) {
    long long n = beans_slen(s);
    BList* b = bytes_mk(n);
    memcpy(b->data, s, (size_t)n);
    return b;
}
long long beans_bytes_len(BList* b) { return b->len; }
long long beans_bytes_eq(BList* a, BList* b) {
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

// unsigned LEB128 over the 64-bit two's-complement pattern (negatives take
// 10 bytes); crc32 is the IEEE polynomial, table-driven — builtins.cpp
// computes the identical table
static void bytes_grow(BList* b, long long need);
void beans_bytes_append_varint(BList* b, long long x) {
    unsigned long long v = (unsigned long long)x;
    while (v >= 0x80) {
        bytes_grow(b, b->len + 1);
        ((unsigned char*)b->data)[b->len++] = (unsigned char)(v | 0x80);
        v >>= 7;
    }
    bytes_grow(b, b->len + 1);
    ((unsigned char*)b->data)[b->len++] = (unsigned char)v;
}
long long beans_bytes_get_varint(BList* b, long long pos, long long line,
                                 long long col) {
    unsigned long long v = 0;
    long long shift = 0;
    long long i = pos < 0 ? b->len : pos;
    while (1) {
        if (pos < 0 || i >= b->len) {
            char m[96];
            snprintf(m, sizeof m, "varint read at %lld out of range (len %lld)", pos,
                     b->len);
            beans_panic(m, line, col);
        }
        if (shift >= 64) {
            char m[96];
            snprintf(m, sizeof m, "varint too long at %lld", pos);
            beans_panic(m, line, col);
        }
        unsigned char byte = ((unsigned char*)b->data)[i++];
        v |= (unsigned long long)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    return (long long)v;
}
long long beans_bytes_varint_size(long long x) {
    unsigned long long v = (unsigned long long)x;
    long long n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}
static unsigned int crc_table[256];
static int crc_ready = 0;
long long beans_bytes_crc32(BList* b, long long from, long long to, long long line,
                            long long col) {
    if (from < 0 || to < from || to > b->len) {
        char m[96];
        snprintf(m, sizeof m, "crc32 %lld..%lld out of range (len %lld)", from, to,
                 b->len);
        beans_panic(m, line, col);
    }
    if (!crc_ready) {
        for (unsigned int i = 0; i < 256; i++) {
            unsigned int c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    unsigned int c = 0xFFFFFFFFu;
    for (long long i = from; i < to; i++) {
        c = crc_table[(c ^ ((unsigned char*)b->data)[i]) & 0xFF] ^ (c >> 8);
    }
    return (long long)(c ^ 0xFFFFFFFFu);
}
static void bytes_grow(BList* b, long long need) {
    if (need <= b->cap) return;
    long long cap = b->cap;
    while (cap < need) cap *= 2;
    b->data = realloc(b->data, (size_t)cap);
    b->cap = cap;
}
void beans_bytes_resize(BList* b, long long n, long long line, long long col) {
    if (n < 0) {
        char m[48];
        snprintf(m, sizeof m, "negative size %lld", n);
        beans_panic(m, line, col);
    }
    bytes_grow(b, n);
    // regrown range reads as zero, like the interpreter's vector resize
    if (n > b->len) memset((char*)b->data + b->len, 0, (size_t)(n - b->len));
    b->len = n;
}
void beans_bytes_reserve(BList* b, long long n, long long line, long long col) {
    if (n < 0) {
        char m[64];
        snprintf(m, sizeof m, "negative reserve capacity %lld", n);
        beans_panic(m, line, col);
    }
    if (n > (1LL << 58)) beans_panic("reserve capacity too large", line, col);
    bytes_grow(b, n);
}
void beans_bytes_fill(BList* b, long long v) {
    memset(b->data, (int)(v & 255), (size_t)b->len);
}
static void bytes_oob(long long i, long long len, long long line, long long col) {
    char m[80];
    snprintf(m, sizeof m, "byte index %lld out of range (len %lld)", i, len);
    beans_panic(m, line, col);
}
long long beans_bytes_get(BList* b, long long i, long long line, long long col) {
    if (i < 0 || i >= b->len) bytes_oob(i, b->len, line, col);
    return (long long)((unsigned char*)b->data)[i];
}
void beans_bytes_set(BList* b, long long i, long long v, long long line, long long col) {
    if (i < 0 || i >= b->len) bytes_oob(i, b->len, line, col);
    ((unsigned char*)b->data)[i] = (unsigned char)(v & 255);
}
static void bytes_woob(const char* what, const char* op, long long pos, long long len,
                       long long line, long long col) {
    char m[96];
    snprintf(m, sizeof m, "%s %s at %lld out of range (len %lld)", what, op, pos, len);
    beans_panic(m, line, col);
}
static long long bytes_getw(BList* b, long long pos, long long w, const char* what,
                            long long line, long long col) {
    // pos > len - w, never pos + w > len: signed overflow on huge pos slips the
    // wrapped sum past the guard and the memcpy goes wild
    if (pos < 0 || w > b->len || pos > b->len - w) bytes_woob(what, "read", pos, b->len, line, col);
    unsigned long long v = 0;
    memcpy(&v, (char*)b->data + pos, (size_t)w); // little-endian hosts, documented
    return (long long)v;
}
static void bytes_putw(BList* b, long long pos, long long w, long long val,
                       const char* what, long long line, long long col) {
    if (pos < 0 || w > b->len || pos > b->len - w) bytes_woob(what, "write", pos, b->len, line, col);
    unsigned long long v = (unsigned long long)val;
    memcpy((char*)b->data + pos, &v, (size_t)w);
}
long long beans_bytes_get_u8(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 1, "u8", l, c); }
long long beans_bytes_get_u16(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 2, "u16", l, c); }
long long beans_bytes_get_u32(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 4, "u32", l, c); }
long long beans_bytes_get_u64(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 8, "u64", l, c); }
long long beans_bytes_get_i64(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 8, "i64", l, c); }
void beans_bytes_put_u8(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 1, v, "u8", l, c); }
void beans_bytes_put_u16(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 2, v, "u16", l, c); }
void beans_bytes_put_u32(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 4, v, "u32", l, c); }
void beans_bytes_put_u64(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 8, v, "u64", l, c); }
void beans_bytes_put_i64(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 8, v, "i64", l, c); }
BList* beans_bytes_slice(BList* b, long long from, long long to, long long line,
                         long long col) {
    if (from < 0 || to < from || to > b->len) {
        char m[96];
        snprintf(m, sizeof m, "slice %lld..%lld out of range (len %lld)", from, to,
                 b->len);
        beans_panic(m, line, col);
    }
    BList* r = bytes_mk(to - from);
    memcpy(r->data, (char*)b->data + from, (size_t)(to - from));
    return r;
}
void beans_bytes_copy_from(BList* b, BList* src, long long at, long long line,
                           long long col) {
    if (at < 0 || src->len > b->len || at > b->len - src->len) {
        char m[96];
        snprintf(m, sizeof m, "copy of %lld bytes at %lld out of range (len %lld)",
                 src->len, at, b->len);
        beans_panic(m, line, col);
    }
    memcpy((char*)b->data + at, src->data, (size_t)src->len);
}
void beans_bytes_append(BList* b, BList* o) {
    bytes_grow(b, b->len + o->len);
    memcpy((char*)b->data + b->len, o->data, (size_t)o->len);
    b->len += o->len;
}
void beans_bytes_append_str(BList* b, char* s) {
    long long n = beans_slen(s);
    bytes_grow(b, b->len + n);
    memcpy((char*)b->data + b->len, s, (size_t)n);
    b->len += n;
}
void beans_bytes_append_i64(BList* b, long long value) {
    bytes_grow(b, b->len + 8);
    unsigned long long bits = (unsigned long long)value;
    for (int shift = 0; shift < 64; shift += 8)
        ((unsigned char*)b->data)[b->len++] = (unsigned char)(bits >> shift);
}
void beans_bytes_append_range(BList* b, BList* source, long long from, long long to,
                              long long line, long long col) {
    if (from < 0 || to < from || to > source->len) {
        char m[96];
        snprintf(m, sizeof m, "slice %lld..%lld out of range (len %lld)", from, to,
                 source->len);
        beans_panic(m, line, col);
    }
    long long count = to - from;
    long long at = b->len;
    bytes_grow(b, at + count);
    memmove((char*)b->data + at, (char*)source->data + from, (size_t)count);
    b->len += count;
}
char* beans_bytes_to_string(BList* b) {
    long long n = 0;
    while (n < b->len && ((char*)b->data)[n] != 0) n++; // strings are text
    return str_make((char*)b->data, n);
}
char* beans_bytes_to_string_full(BList* b) {
    return str_make((char*)b->data, b->len);
}

// ---- files (kind 6 resources) -----------------------------------------------
// errno -> Error.kind slug; the interpreter builds the identical pair
static const char* fs_kind_of(int err) {
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
static void* fs_err_obj(const char* path, int err) {
    size_t n = strlen(path) + 96;
    char* b = malloc(n);
    snprintf(b, n, "%s: %s", path, strerror(err));
    void* e = mk_error(b, fs_kind_of(err));
    free(b);
    return e;
}
// for paths that are beans strings: splice by stored length so a path with
// an embedded NUL reports the same bytes the interpreter does (the plain
// fs_err_obj above stays for internal C-built paths, which never hold NUL)
static void* fs_err_obj_rc(char* path, int err) {
    const char* es = strerror(err);
    long long lp = beans_slen(path);
    size_t le = strlen(es);
    long long total = lp + 2 + (long long)le;
    char* m = beans_alloc(total + 1, total << 3);
    memcpy(m, path, (size_t)lp);
    m[lp] = ':';
    m[lp + 1] = ' ';
    memcpy(m + lp + 2, es, le);
    return mk_error_own(m, fs_kind_of(err));
}
static void* op_err_obj(const char* op, int err) {
    char b[96];
    snprintf(b, sizeof b, "%s: %s", op, strerror(err));
    return mk_error(b, fs_kind_of(err));
}
static void* closed_err(void) { return mk_error("file is closed", "closed"); }

BRes beans_file_read_at(BFile* f, long long pos, long long n) {
    if (f->closed) return (BRes){0, closed_err()};
    if (pos < 0 || n < 0) return (BRes){0, mk_error("negative read", "io")};
    // clamp to what the file can actually give: a corrupted length field must
    // not become a giant allocation — the read comes back short anyway
    struct stat rst;
    if (fstat((int)f->fd, &rst) == 0 && S_ISREG(rst.st_mode)) {
        long long rem = rst.st_size > pos ? rst.st_size - pos : 0;
        if (n > rem) n = rem;
    }
    BList* buf = bytes_mk(n);
    long long got = 0;
    while (got < n) {
        ssize_t r = pread((int)f->fd, (char*)buf->data + got, (size_t)(n - got),
                          (off_t)(pos + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            beans_release(buf); // the error path must not leak the buffer
            return (BRes){0, op_err_obj("read", e)};
        }
        if (r == 0) break; // eof: a short read returns what's there
        got += r;
    }
    buf->len = got;
    return (BRes){(long long)buf, NULL};
}
BRes beans_file_write_at(BFile* f, long long pos, BList* d) {
    if (f->closed) return (BRes){0, closed_err()};
    if (pos < 0) return (BRes){0, mk_error("negative write", "io")};
    long long done = 0;
    while (done < d->len) {
        ssize_t r = pwrite((int)f->fd, (char*)d->data + done, (size_t)(d->len - done),
                           (off_t)(pos + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return (BRes){0, op_err_obj("write", errno)};
        }
        done += r;
    }
    return (BRes){done, NULL};
}
BRes beans_file_read(BFile* f, long long n) {
    if (f->closed) return (BRes){0, closed_err()};
    if (n < 0) return (BRes){0, mk_error("negative read", "io")};
    struct stat rst;
    if (fstat((int)f->fd, &rst) == 0 && S_ISREG(rst.st_mode)) {
        long long at = (long long)lseek((int)f->fd, 0, SEEK_CUR);
        long long rem = at >= 0 && rst.st_size > at ? rst.st_size - at : 0;
        if (n > rem) n = rem;
    }
    BList* buf = bytes_mk(n);
    long long got = 0;
    while (got < n) {
        ssize_t r = read((int)f->fd, (char*)buf->data + got, (size_t)(n - got));
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            beans_release(buf); // the error path must not leak the buffer
            return (BRes){0, op_err_obj("read", e)};
        }
        if (r == 0) break;
        got += r;
    }
    buf->len = got;
    return (BRes){(long long)buf, NULL};
}
BRes beans_file_write(BFile* f, BList* d) {
    if (f->closed) return (BRes){0, closed_err()};
    long long done = 0;
    while (done < d->len) {
        ssize_t r = write((int)f->fd, (char*)d->data + done, (size_t)(d->len - done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return (BRes){0, op_err_obj("write", errno)};
        }
        done += r;
    }
    return (BRes){done, NULL};
}
long long beans_file_seek(BFile* f, long long pos, long long line, long long col) {
    if (f->closed) beans_panic("file is closed", line, col);
    off_t r = lseek((int)f->fd, (off_t)pos, SEEK_SET);
    if (r < 0) {
        char m[96];
        snprintf(m, sizeof m, "seek to %lld: %s", pos, strerror(errno));
        beans_panic(m, line, col);
    }
    return (long long)r;
}
long long beans_file_seek_end(BFile* f, long long off, long long line, long long col) {
    if (f->closed) beans_panic("file is closed", line, col);
    off_t r = lseek((int)f->fd, (off_t)off, SEEK_END);
    if (r < 0) {
        char m[96];
        snprintf(m, sizeof m, "seek to %lld: %s", off, strerror(errno));
        beans_panic(m, line, col);
    }
    return (long long)r;
}
long long beans_file_tell(BFile* f, long long line, long long col) {
    if (f->closed) beans_panic("file is closed", line, col);
    return (long long)lseek((int)f->fd, 0, SEEK_CUR);
}
BRes beans_file_size(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    struct stat st;
    if (fstat((int)f->fd, &st) != 0) return (BRes){0, op_err_obj("size", errno)};
    return (BRes){(long long)st.st_size, NULL};
}
BRes beans_file_truncate(BFile* f, long long n) {
    if (f->closed) return (BRes){0, closed_err()};
    if (ftruncate((int)f->fd, (off_t)n) != 0) {
        return (BRes){0, op_err_obj("truncate", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_file_sync(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    if (fsync((int)f->fd) != 0) return (BRes){0, op_err_obj("sync", errno)};
    return (BRes){1, NULL};
}
BRes beans_file_close(BFile* f) {
    if (f->closed) return (BRes){0, mk_error("file already closed", "closed")};
    f->closed = 1;
    // While worker threads are live, defer the real close(): a racing op on
    // another thread could still be mid-syscall on this fd, and reusing the
    // number for a freshly-opened file would silently corrupt it. The fd stays
    // open (harmless — same file) until the handle's last ref drops in
    // cc_free_shell, when no thread can hold it. This mirrors the collector's
    // own "don't touch shared resources while mutators run" gate. Zero cost
    // single-threaded, where cc_threads is 0 and the fd closes now.
    if (cc_threads > 0) return (BRes){1, NULL};
    long long fd = f->fd;
    f->fd = -1;
    if (close((int)fd) != 0) return (BRes){0, op_err_obj("close", errno)};
    return (BRes){1, NULL};
}

// advisory flock — single-writer databases; try_lock's ok(false) means "held
// by someone else", every other failure is a real error
BRes beans_file_lock(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    // a blocking lock is the classic EINTR victim — retry rather than fail
    while (flock((int)f->fd, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        return (BRes){0, op_err_obj("lock", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_file_try_lock(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    if (flock((int)f->fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) return (BRes){0, NULL};
        return (BRes){0, op_err_obj("try_lock", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_file_unlock(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    if (flock((int)f->fd, LOCK_UN) != 0) return (BRes){0, op_err_obj("unlock", errno)};
    return (BRes){1, NULL};
}

// ---- mmap (kind 6, shape bit 0 = 1) ----
// whole-file shared mapping; the fd is kept open so resize() can ftruncate +
// remap. get/put/read/write panic on a closed or short map, flush/close
// report errors as Results like File does.
static void* mmap_closed_err(void) { return mk_error("mmap is closed", "closed"); }
BRes beans_mmap_open(char* path, long long writable) {
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return (BRes){0, fs_err_obj_rc(path, e)};
    }
    char* p = NULL;
    if (st.st_size > 0) {
        p = mmap(NULL, (size_t)st.st_size,
                 writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            int e = errno;
            close(fd);
            return (BRes){0, fs_err_obj_rc(path, e)};
        }
    }
    BMMap* m = beans_alloc(sizeof(BMMap), 6 | (1 << 3));
    m->p = p;
    m->len = (long long)st.st_size;
    m->fd = fd; // kept: resize() needs it to ftruncate + remap
    m->writable = writable;
    return (BRes){(long long)m, NULL};
}
long long beans_mmap_len(BMMap* m) { return m->len; }
static void mmap_guard(BMMap* m, long long line, long long col) {
    if (m->closed) beans_panic("mmap is closed", line, col);
}
static long long mmap_word(BMMap* m, const char* what, long long pos, long long w,
                           long long line, long long col) {
    mmap_guard(m, line, col);
    // pos > len - w, never pos + w > len — the sum overflows for huge pos
    if (pos < 0 || w > m->len || pos > m->len - w) {
        char b[96];
        snprintf(b, sizeof b, "%s read at %lld out of range (len %lld)", what, pos,
                 m->len);
        beans_panic(b, line, col);
    }
    unsigned long long v = 0;
    memcpy(&v, m->p + pos, (size_t)w);
    return (long long)v;
}
static void mmap_put_word(BMMap* m, const char* what, long long pos, long long v,
                          long long w, long long line, long long col) {
    mmap_guard(m, line, col);
    if (!m->writable) beans_panic("mmap is read-only", line, col);
    if (pos < 0 || w > m->len || pos > m->len - w) {
        char b[96];
        snprintf(b, sizeof b, "%s write at %lld out of range (len %lld)", what, pos,
                 m->len);
        beans_panic(b, line, col);
    }
    memcpy(m->p + pos, &v, (size_t)w);
}
long long beans_mmap_get_u8(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u8", p, 1, l, c); }
long long beans_mmap_get_u16(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u16", p, 2, l, c); }
long long beans_mmap_get_u32(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u32", p, 4, l, c); }
long long beans_mmap_get_u64(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u64", p, 8, l, c); }
long long beans_mmap_get_i64(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "i64", p, 8, l, c); }
void beans_mmap_put_u8(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u8", p, v, 1, l, c); }
void beans_mmap_put_u16(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u16", p, v, 2, l, c); }
void beans_mmap_put_u32(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u32", p, v, 4, l, c); }
void beans_mmap_put_u64(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u64", p, v, 8, l, c); }
void beans_mmap_put_i64(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "i64", p, v, 8, l, c); }
BList* beans_mmap_read(BMMap* m, long long pos, long long n, long long line,
                       long long col) {
    mmap_guard(m, line, col);
    if (pos < 0 || n < 0 || n > m->len || pos > m->len - n) {
        char b[96];
        snprintf(b, sizeof b, "read %lld at %lld out of range (len %lld)", n, pos,
                 m->len);
        beans_panic(b, line, col);
    }
    BList* r = bytes_mk(n);
    memcpy(r->data, m->p + pos, (size_t)n);
    return r;
}
void beans_mmap_write(BMMap* m, long long pos, BList* d, long long line,
                      long long col) {
    mmap_guard(m, line, col);
    if (!m->writable) beans_panic("mmap is read-only", line, col);
    if (pos < 0 || d->len > m->len || pos > m->len - d->len) {
        char b[96];
        snprintf(b, sizeof b, "write %lld at %lld out of range (len %lld)", d->len,
                 pos, m->len);
        beans_panic(b, line, col);
    }
    memcpy(m->p + pos, d->data, (size_t)d->len);
}
BRes beans_mmap_flush(BMMap* m) {
    if (m->closed) return (BRes){0, mmap_closed_err()};
    if (m->len > 0 && msync(m->p, (size_t)m->len, MS_SYNC) != 0) {
        return (BRes){0, op_err_obj("flush", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_mmap_flush_range(BMMap* m, long long pos, long long n) {
    if (m->closed) return (BRes){0, mmap_closed_err()};
    if (pos < 0 || n < 0 || n > m->len || pos > m->len - n) {
        char b[96];
        snprintf(b, sizeof b, "flush %lld at %lld out of range (len %lld)", n, pos,
                 m->len);
        return (BRes){0, mk_error(b, "io")};
    }
    if (n > 0) {
        long long page = (long long)getpagesize();
        long long start = pos - pos % page; // msync wants a page-aligned base
        if (msync(m->p + start, (size_t)(pos + n - start), MS_SYNC) != 0) {
            return (BRes){0, op_err_obj("flush", errno)};
        }
    }
    return (BRes){1, NULL};
}
BRes beans_mmap_close(BMMap* m) {
    if (m->closed) return (BRes){0, mk_error("mmap already closed", "closed")};
    m->closed = 1;
    // defer munmap+close while workers run (see beans_file_close): a racing op
    // reading through the mapping must not have it pulled out from under it
    if (cc_threads > 0) return (BRes){1, NULL};
    int bad = m->p && munmap(m->p, (size_t)m->len) != 0;
    int e = errno;
    m->p = NULL;
    if (m->fd >= 0) close((int)m->fd);
    m->fd = -1;
    if (bad) return (BRes){0, op_err_obj("close", e)};
    return (BRes){1, NULL};
}

// grow or shrink in place: truncate the file, drop the old mapping, map
// fresh. On a mapping failure the handle stays open but empty (len 0).
BRes beans_mmap_resize(BMMap* m, long long n) {
    if (m->closed) return (BRes){0, mmap_closed_err()};
    if (!m->writable) return (BRes){0, mk_error("mmap is read-only", "permission")};
    if (n < 0) return (BRes){0, mk_error("negative resize", "io")};
    if (ftruncate((int)m->fd, (off_t)n) != 0) {
        return (BRes){0, op_err_obj("resize", errno)};
    }
    if (m->p) munmap(m->p, (size_t)m->len);
    m->p = NULL;
    m->len = 0;
    if (n > 0) {
        char* p = mmap(NULL, (size_t)n, PROT_READ | PROT_WRITE, MAP_SHARED,
                       (int)m->fd, 0);
        if (p == MAP_FAILED) return (BRes){0, op_err_obj("resize", errno)};
        m->p = p;
        m->len = n;
    }
    return (BRes){1, NULL};
}

// ---- Dir.walk: files and symlinks under root (lstat — never follows a
// link), paths relative to root, "/"-joined, sorted at the end ----
typedef struct {
    char** v;
    long long len, cap;
} StrVec;
static void sv_push(StrVec* s, char* p) {
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = realloc(s->v, (size_t)s->cap * sizeof(char*));
    }
    s->v[s->len++] = p;
}
static char* path_cat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* r = malloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb);
    r[la + 1 + lb] = 0;
    return r;
}
static void sv_free(StrVec* s) {
    for (long long i = 0; i < s->len; i++) free(s->v[i]);
    free(s->v);
}
// Iterative walk: an explicit stack of relative dir paths, one DIR open at a
// time. Recursion held one fd per depth level and ran out at ~250 deep; this
// caps open fds at one. Output is sorted afterward, so traversal order is
// free.
static int walk_dir(const char* root, StrVec* out, char** epath, int* eno) {
    StrVec stack = {0, 0, 0};
    sv_push(&stack, strdup("")); // "" = root itself
    int ok = 1;
    while (stack.len > 0) {
        char* rel = stack.v[--stack.len];
        char* full = rel[0] ? path_cat(root, rel) : strdup(root);
        DIR* d = opendir(full);
        if (!d) {
            *epath = full;
            *eno = errno;
            free(rel);
            ok = 0;
            break;
        }
        free(full);
        struct dirent* en;
        while ((en = readdir(d))) {
            if (strcmp(en->d_name, ".") == 0 || strcmp(en->d_name, "..") == 0) continue;
            char* r2 = rel[0] ? path_cat(rel, en->d_name) : strdup(en->d_name);
            char* abs = path_cat(root, r2);
            struct stat st;
            if (lstat(abs, &st) != 0) {
                *epath = abs;
                *eno = errno;
                free(r2);
                ok = 0;
                break;
            }
            free(abs);
            if (S_ISDIR(st.st_mode)) sv_push(&stack, r2); // descend later
            else sv_push(out, r2);
        }
        closedir(d);
        free(rel);
        if (!ok) break;
    }
    for (long long i = 0; i < stack.len; i++) free(stack.v[i]);
    free(stack.v);
    return ok;
}
static int walk_cmp(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}
BRes beans_dir_walk(char* path) {
    StrVec out = {0, 0, 0};
    char* epath = NULL;
    int eno = 0;
    if (!walk_dir(path, &out, &epath, &eno)) {
        void* e = fs_err_obj(epath, eno);
        free(epath);
        for (long long i = 0; i < out.len; i++) free(out.v[i]);
        free(out.v);
        return (BRes){0, e};
    }
    qsort(out.v, (size_t)out.len, sizeof(char*), walk_cmp);
    BList* l = beans_list_new(1);
    for (long long i = 0; i < out.len; i++) {
        beans_list_push(l, (long long)rc_strdup(out.v[i]));
        free(out.v[i]);
    }
    free(out.v);
    return (BRes){(long long)l, NULL};
}

BRes beans_file_open(char* path, char* mode) {
    int flags;
    if (strcmp(mode, "r") == 0) flags = O_RDONLY;
    else if (strcmp(mode, "rw") == 0) flags = O_RDWR;
    else if (strcmp(mode, "create") == 0) flags = O_RDWR | O_CREAT;
    else if (strcmp(mode, "append") == 0) flags = O_WRONLY | O_CREAT | O_APPEND;
    else {
        // mode is a beans string of any length — heap-build so a long bad
        // mode reports its full text like the interpreter, not a truncation
        size_t n = strlen(mode) + 24;
        char* m = malloc(n);
        snprintf(m, n, "bad open mode '%s'", mode);
        void* e = mk_error(m, "io");
        free(m);
        return (BRes){0, e};
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    BFile* f = beans_alloc(sizeof(BFile), 6);
    f->fd = fd;
    f->closed = 0;
    return (BRes){(long long)f, NULL};
}

long long beans_file_exists(char* path) {
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}
BRes beans_file_size_p(char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){(long long)st.st_size, NULL};
}
BRes beans_file_remove(char* path) {
    struct stat st;
    // lstat: remove the link itself, and let a dangling symlink be removed
    if (lstat(path, &st) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    int r = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (r != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
BRes beans_file_rename(char* from, char* to) {
    if (rename(from, to) != 0) return (BRes){0, fs_err_obj_rc(from, errno)};
    return (BRes){1, NULL};
}
BRes beans_dir_make(char* path) {
    if (mkdir(path, 0755) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
BRes beans_dir_make_all(char* path) {
    long long n = beans_slen(path);
    char* cur = malloc((size_t)n + 1);
    long long i = 0;
    while (i < n) {
        long long j = i;
        while (j < n && path[j] != '/') j++;
        memcpy(cur, path, (size_t)j);
        cur[j] = 0;
        i = j + 1;
        if (cur[0] == 0) continue;
        if (mkdir(cur, 0755) != 0) {
            int me = errno;
            // EEXIST is only ok if the existing entry is a directory
            struct stat st;
            if (me != EEXIST || stat(cur, &st) != 0 || !S_ISDIR(st.st_mode)) {
                BRes r = {0, fs_err_obj(cur, me == EEXIST ? ENOTDIR : me)};
                free(cur);
                return r;
            }
        }
    }
    free(cur);
    return (BRes){1, NULL};
}
static int name_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}
BRes beans_dir_list(char* path) {
    DIR* d = opendir(path);
    if (!d) return (BRes){0, fs_err_obj_rc(path, errno)};
    char** names = NULL;
    long long cnt = 0, cap = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 16;
            names = realloc(names, (size_t)cap * sizeof(char*));
        }
        names[cnt++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)cnt, sizeof(char*), name_cmp); // deterministic
    BList* l = beans_list_new(1);
    for (long long i = 0; i < cnt; i++) {
        beans_list_push(l, (long long)rc_strdup(names[i]));
        free(names[i]);
    }
    free(names);
    return (BRes){(long long)l, NULL};
}
BRes beans_dir_remove(char* path) {
    if (rmdir(path) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
// Iterative delete: gather the tree pre-order (parent before child) with one
// DIR open at a time, then remove in reverse (deepest first). Recursion held
// one fd per level and a fixed 1024-byte path buffer that silently truncated
// deep paths; this heap-builds every path and caps open fds at one.
static int rm_tree(const char* p) {
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    StrVec dirs = {0, 0, 0}; // dirs to rmdir, in discovery (pre) order
    StrVec stack = {0, 0, 0}; // dirs still to scan
    sv_push(&stack, strdup(p));
    int ok = 1;
    while (stack.len > 0) {
        char* dir = stack.v[--stack.len];
        DIR* d = opendir(dir);
        if (!d) {
            free(dir);
            ok = 0;
            break;
        }
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char* sub = path_cat(dir, de->d_name);
            struct stat cst;
            if (lstat(sub, &cst) != 0) {
                free(sub);
                ok = 0;
                break;
            }
            if (S_ISDIR(cst.st_mode)) sv_push(&stack, sub); // scan later
            else {
                if (unlink(sub) != 0) ok = 0;
                free(sub);
                if (!ok) break;
            }
        }
        closedir(d);
        sv_push(&dirs, dir); // remove this dir after its children
        if (!ok) break;
    }
    for (long long i = 0; i < stack.len; i++) free(stack.v[i]);
    free(stack.v);
    // deepest first: dirs were collected parent-before-child, so reverse
    for (long long i = dirs.len; ok && i-- > 0;) {
        if (rmdir(dirs.v[i]) != 0) ok = 0;
    }
    sv_free(&dirs);
    return ok ? 0 : -1;
}
BRes beans_dir_remove_all(char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    if (rm_tree(path) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
long long beans_dir_exists(char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
char* beans_dir_temp(void) {
    const char* t = getenv("TMPDIR");
    const char* src = t && *t ? t : "/tmp";
    long long n = (long long)strlen(src);
    while (n > 1 && src[n - 1] == '/') n--; // trim trailing slashes
    return str_make(src, n);
}
BRes beans_dir_sync(char* path) {
    // the database commit pattern: fsync the directory after a rename
    int fd = open(path, O_RDONLY);
    if (fd < 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    if (fsync(fd) != 0) {
        int e = errno;
        close(fd);
        return (BRes){0, fs_err_obj_rc(path, e)};
    }
    close(fd);
    return (BRes){1, NULL};
}

// ---- std.os / std.io --------------------------------------------------------
static int os_argc;
static char** os_argv;
__attribute__((constructor)) static void os_capture(int argc, char** argv) {
    os_argc = argc;
    os_argv = argv;
}
BList* beans_os_args(void) {
    BList* l = beans_list_new(1);
    for (int i = 1; i < os_argc; i++) {
        beans_list_push(l, (long long)rc_strdup(os_argv[i]));
    }
    return l;
}
BOpt beans_os_env(char* name) {
    const char* v = getenv(name);
    if (!v) return (BOpt){0, 0};
    return (BOpt){(long long)rc_strdup(v), 1};
}
void beans_os_exit(long long code) { exit((int)code); }
long long beans_os_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
long long beans_os_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void beans_os_sleep_ms(long long ms) {
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }
}
BOpt beans_io_read_line(void) {
    char* out = NULL;
    long long len = 0, cap = 0;
    int c;
    int any = 0;
    while ((c = fgetc(stdin)) != EOF) {
        any = 1;
        if (c == '\n') break;
        if (len == cap) {
            cap = cap ? cap * 2 : 128;
            out = realloc(out, (size_t)cap);
        }
        out[len++] = (char)c;
    }
    if (!any) {
        free(out);
        return (BOpt){0, 0};
    }
    char* s = str_make(out ? out : "", len);
    free(out);
    return (BOpt){(long long)s, 1};
}
char* beans_io_read_all(void) {
    char* out = NULL;
    long long len = 0, cap = 0;
    char chunk[65536];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        if (len + (long long)r > cap) {
            cap = cap ? cap * 2 : 65536;
            while (cap < len + (long long)r) cap *= 2;
            out = realloc(out, (size_t)cap);
        }
        memcpy(out + len, chunk, r);
        len += r;
    }
    char* s = str_make(out ? out : "", len);
    free(out);
    return s;
}
void beans_eprintln(char* s) {
    fwrite(s, 1, (size_t)beans_slen(s), stderr);
    fputc('\n', stderr);
}
void beans_eprint(char* s) { fwrite(s, 1, (size_t)beans_slen(s), stderr); }

// ---- threads ----
typedef struct {
    pthread_t th;
    long long result;
    long long (*thunk)(void*);
    void* env;
    int joined;
} BThread;
void beans_thread_release_env(void* env);
static void* thread_main(void* arg) {
    BThread* t = arg;
    t->result = t->thunk(t->env);
    beans_release(t->env);
    beans_release(t); // the running thread's own ref on the handle
    // last heap touch is done — the cycle collector may run again
    cc_threads -= 1;
    return NULL;
}
BThread* beans_thread_spawn(void* thunk, void* env) {
    BThread* t = beans_alloc(sizeof(BThread), 0);
    t->thunk = (long long (*)(void*))thunk;
    t->env = env; // ownership of the closure box moves to the thread
    cc_enable_mt(); // from here every count op is atomic, in every thread
    beans_retain(t); // one ref for the handle, one for the running thread
    cc_threads += 1;
    pthread_create(&t->th, NULL, thread_main, t);
    return t;
}
long long beans_thread_join(BThread* t) {
    if (t->joined) beans_panic("thread already joined", 0, 0);
    t->joined = 1;
    pthread_join(t->th, NULL);
    return t->result;
}

BMutex* beans_mutex_new(long long inner, long long inner_ptr) {
    BMutex* mu = beans_alloc(sizeof(BMutex), 5 | (inner_ptr << 3));
    pthread_mutex_init(&mu->m, NULL);
    mu->inner = inner;
    return mu;
}
long long beans_mutex_lock(BMutex* mu) {
    pthread_mutex_lock(&mu->m);
    return mu->inner;
}
void beans_mutex_unlock(BMutex* mu) { pthread_mutex_unlock(&mu->m); }

BChan* beans_chan_new(long long cap, long long elem_ptr) {
    BChan* c = beans_alloc(sizeof(BChan), 4 | (elem_ptr << 3));
    c->cap = cap > 0 ? cap : 1;
    c->q = calloc((size_t)c->cap, 8);
    pthread_mutex_init(&c->m, NULL);
    pthread_cond_init(&c->can_send, NULL);
    pthread_cond_init(&c->can_recv, NULL);
    return c;
}
long long beans_chan_send(BChan* c, long long v) {
    pthread_mutex_lock(&c->m);
    while (c->count == c->cap && !c->closed) pthread_cond_wait(&c->can_send, &c->m);
    if (c->closed) {
        pthread_mutex_unlock(&c->m);
        return 0; // caller panics; caller also still owns v
    }
    c->q[(c->head + c->count) % c->cap] = v;
    c->count += 1;
    pthread_cond_signal(&c->can_recv);
    pthread_mutex_unlock(&c->m);
    return 1;
}
long long beans_chan_recv(BChan* c, long long* ok) {
    pthread_mutex_lock(&c->m);
    while (c->count == 0 && !c->closed) pthread_cond_wait(&c->can_recv, &c->m);
    if (c->count == 0) {
        *ok = 0;
        pthread_mutex_unlock(&c->m);
        return 0;
    }
    long long v = c->q[c->head];
    c->head = (c->head + 1) % c->cap;
    c->count -= 1;
    *ok = 1;
    pthread_cond_signal(&c->can_send);
    pthread_mutex_unlock(&c->m);
    return v;
}
void beans_chan_close(BChan* c) {
    pthread_mutex_lock(&c->m);
    c->closed = 1;
    pthread_cond_broadcast(&c->can_send);
    pthread_cond_broadcast(&c->can_recv);
    pthread_mutex_unlock(&c->m);
}

typedef struct { _Atomic long long v; } BAtomic;
BAtomic* beans_atomic_new(long long init) {
    BAtomic* a = beans_alloc(sizeof(BAtomic), 0);
    a->v = init;
    return a;
}
long long beans_atomic_add(BAtomic* a, long long d) { return (a->v += d); }
long long beans_atomic_get(BAtomic* a) { return a->v; }
void beans_atomic_set(BAtomic* a, long long v) { a->v = v; }

// ---- decimal: 128-bit coefficient + base-10 scale (same math as the interpreter) ----
typedef struct BDec {
    __int128 c;
    long long s;
} BDec;
typedef unsigned __int128 BDecV;
#define BDEC_COEFF_BITS 112
static BDecV decv_coeff_mask(void) {
    return (((BDecV)1) << BDEC_COEFF_BITS) - 1;
}
static __int128 decv_coeff(BDecV v) {
    BDecV mask = decv_coeff_mask();
    BDecV raw = v & mask;
    if (raw & (((BDecV)1) << (BDEC_COEFF_BITS - 1))) raw |= ~mask;
    return (__int128)raw;
}
static long long decv_scale(BDecV v) {
    return (long long)(v >> BDEC_COEFF_BITS);
}
static BDecV decv_mk(__int128 c, long long s) {
    const __int128 limit = ((__int128)1) << (BDEC_COEFF_BITS - 1);
    if (s < 0 || s > 65535 || c < -limit || c >= limit)
        beans_panic("decimal overflow", 0, 0);
    return (((BDecV)(unsigned long long)s) << BDEC_COEFF_BITS) |
           ((BDecV)c & decv_coeff_mask());
}
static BDec* dec_mk(__int128 c, long long s) {
    BDec* d = beans_alloc(sizeof(BDec), 0);
    d->c = c;
    d->s = s;
    return d;
}
static __int128 pow10i(long long n) {
    __int128 p = 1;
    for (long long i = 0; i < n; i++) p *= 10;
    return p;
}
BDec* beans_dec_new(__int128 c, long long s) { return dec_mk(c, s); }
BDec* beans_dec_from_int(long long v) { return dec_mk((__int128)v, 0); }
BDec* beans_decv_box(BDecV v) { return dec_mk(decv_coeff(v), decv_scale(v)); }
BDecV beans_decv_unbox(BDec* v) { return decv_mk(v->c, v->s); }
BDecV beans_decv_from_int(long long v) { return decv_mk((__int128)v, 0); }
static void dec_align(BDec* a, BDec* b, __int128* ca, __int128* cb, long long* s) {
    *s = a->s > b->s ? a->s : b->s;
    *ca = a->c * pow10i(*s - a->s);
    *cb = b->c * pow10i(*s - b->s);
}
static void decv_align(BDecV a, BDecV b, __int128* ca, __int128* cb,
                       long long* s) {
    long long as = decv_scale(a), bs = decv_scale(b);
    *s = as > bs ? as : bs;
    *ca = decv_coeff(a) * pow10i(*s - as);
    *cb = decv_coeff(b) * pow10i(*s - bs);
}
__attribute__((always_inline)) BDecV beans_decv_add(BDecV a, BDecV b) {
    if (decv_scale(a) == decv_scale(b))
        return decv_mk(decv_coeff(a) + decv_coeff(b), decv_scale(a));
    __int128 ca, cb;
    long long s;
    decv_align(a, b, &ca, &cb, &s);
    return decv_mk(ca + cb, s);
}
__attribute__((always_inline)) BDecV beans_decv_sub(BDecV a, BDecV b) {
    if (decv_scale(a) == decv_scale(b))
        return decv_mk(decv_coeff(a) - decv_coeff(b), decv_scale(a));
    __int128 ca, cb;
    long long s;
    decv_align(a, b, &ca, &cb, &s);
    return decv_mk(ca - cb, s);
}
BDecV beans_decv_mul(BDecV a, BDecV b) {
    return decv_mk(decv_coeff(a) * decv_coeff(b),
                   decv_scale(a) + decv_scale(b));
}
BDecV beans_decv_div(BDecV a, BDecV b, long long ln, long long cl) {
    __int128 bc = decv_coeff(b);
    if (bc == 0) beans_panic("divide by zero", ln, cl);
    long long extra = 20;
    __int128 q = decv_coeff(a) * pow10i(extra + decv_scale(b)) / bc;
    long long s = decv_scale(a) + extra;
    while (s > 0 && q % 10 == 0) {
        q /= 10;
        s -= 1;
    }
    return decv_mk(q, s);
}
BDecV beans_decv_neg(BDecV a) {
    return decv_mk(-decv_coeff(a), decv_scale(a));
}
BDecV beans_decv_abs(BDecV a) {
    __int128 c = decv_coeff(a);
    return decv_mk(c < 0 ? -c : c, decv_scale(a));
}
BDecV beans_decv_round(BDecV a, long long places) {
    __int128 c = decv_coeff(a);
    long long s = decv_scale(a);
    if (places >= s) return a;
    __int128 f = pow10i(s - places);
    __int128 q = c / f, rem = c % f;
    if (rem < 0) rem = -rem;
    if (rem * 2 >= f) q += c >= 0 ? 1 : -1;
    if (places < 0) return decv_mk(q * pow10i(-places), 0);
    return decv_mk(q, places);
}
int beans_decv_cmp(BDecV a, BDecV b) {
    __int128 ca, cb;
    long long s;
    decv_align(a, b, &ca, &cb, &s);
    return ca < cb ? -1 : ca > cb ? 1 : 0;
}
BDec* beans_dec_add(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return dec_mk(ca + cb, s);
}
BDec* beans_dec_sub(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return dec_mk(ca - cb, s);
}
BDec* beans_dec_mul(BDec* a, BDec* b) { return dec_mk(a->c * b->c, a->s + b->s); }
BDec* beans_dec_div(BDec* a, BDec* b, long long ln, long long cl) {
    if (b->c == 0) beans_panic("divide by zero", ln, cl);
    long long extra = 20;
    __int128 num = a->c * pow10i(extra + b->s);
    __int128 q = num / b->c;
    long long s = a->s + extra;
    while (s > 0 && q % 10 == 0) {
        q /= 10;
        s -= 1;
    }
    return dec_mk(q, s);
}
BDec* beans_dec_neg(BDec* a) { return dec_mk(-a->c, a->s); }
BDec* beans_dec_abs(BDec* a) { return dec_mk(a->c < 0 ? -a->c : a->c, a->s); }
BDec* beans_dec_round(BDec* a, long long places) {
    if (places >= a->s) return dec_mk(a->c, a->s);
    __int128 f = pow10i(a->s - places);
    __int128 q = a->c / f, rem = a->c % f;
    if (rem < 0) rem = -rem;
    if (rem * 2 >= f) q += a->c >= 0 ? 1 : -1;
    // negative places round to a power of ten: scale the whole-number result
    // back up and keep scale 0 (matches Decimal::round_to)
    if (places < 0) return dec_mk(q * pow10i(-places), 0);
    return dec_mk(q, places);
}
int beans_dec_cmp(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return ca < cb ? -1 : ca > cb ? 1 : 0;
}
// dec_cmp aligns scales, so 2.50 == 2.5 — hash the canonical trailing-zero-free
// form so equal decimals land in the same map index slot
long long beans_dec_hash(BDec* d) {
    __int128 c = d->c;
    long long s = d->s;
    while (s > 0 && c % 10 == 0) {
        c /= 10;
        s -= 1;
    }
    unsigned long long lo = (unsigned long long)(unsigned __int128)c;
    unsigned long long hi = (unsigned long long)((unsigned __int128)c >> 64);
    return (long long)beans_mix64(lo ^ beans_mix64(hi ^ (unsigned long long)s));
}
// same acceptance rule as the interpreter's dec_valid: [+-]? digits with '_',
// one optional '.', one optional e/E exponent with its own sign and a digit
static int dec_valid_c(const char* s) {
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
            if (!(s[i] >= '0' && s[i] <= '9')) return 0;
            // capped at 4096 exactly like the interpreter's dec_valid
            long long ev = 0;
            while (s[i] >= '0' && s[i] <= '9') {
                ev = ev * 10 + (s[i] - '0');
                if (ev > 4096) return 0;
                i++;
            }
            return s[i] == '\0';
        }
        return 0;
    }
    return digits > 0;
}
// mirror of Decimal::parse — the two must compute identically
BRes beans_str_to_decimal(char* s) {
    if (!dec_valid_c(s)) return parse_fail(s, "decimal");
    __int128 coeff = 0;
    long long scale = 0, exp = 0;
    int neg = 0, after_dot = 0;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') {
        neg = s[i] == '-';
        i++;
    }
    for (; s[i]; i++) {
        char c = s[i];
        if (c == '_') continue;
        if (c == '.') { after_dot = 1; continue; }
        if (c == 'e' || c == 'E') {
            exp = strtol(s + i + 1, NULL, 10);
            break;
        }
        coeff = coeff * 10 + (c - '0');
        if (after_dot) scale += 1;
    }
    scale -= exp;
    if (scale < 0) {
        coeff *= pow10i(-scale);
        scale = 0;
    }
    if (neg) coeff = -coeff;
    return (BRes){(long long)dec_mk(coeff, scale), NULL};
}
long long beans_dec_to_int(BDec* a) { return (long long)(a->c / pow10i(a->s)); }
double beans_dec_to_f64(BDec* a) { return (double)a->c / (double)pow10i(a->s); }
long long beans_decv_to_int(BDecV a) {
    return (long long)(decv_coeff(a) / pow10i(decv_scale(a)));
}
double beans_decv_to_f64(BDecV a) {
    return (double)decv_coeff(a) / (double)pow10i(decv_scale(a));
}
BDec* beans_dec_from_f64(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    __int128 c = 0;
    long long s = 0;
    int neg = 0, after = 0;
    long long ex = 0;
    for (const char* p = buf; *p; p++) {
        if (*p == '-') { neg = 1; continue; }
        if (*p == '+') continue;
        if (*p == '.') { after = 1; continue; }
        if (*p == 'e' || *p == 'E') {
            ex = strtoll(p + 1, NULL, 10);
            break;
        }
        c = c * 10 + (*p - '0');
        if (after) s += 1;
    }
    s -= ex;
    if (s < 0) {
        c *= pow10i(-s);
        s = 0;
    }
    if (neg) c = -c;
    return dec_mk(c, s);
}
BDecV beans_decv_from_f64(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    __int128 c = 0;
    long long s = 0;
    int neg = 0, after = 0;
    long long ex = 0;
    for (const char* p = buf; *p; p++) {
        if (*p == '-') { neg = 1; continue; }
        if (*p == '+') continue;
        if (*p == '.') { after = 1; continue; }
        if (*p == 'e' || *p == 'E') {
            ex = strtoll(p + 1, NULL, 10);
            break;
        }
        c = c * 10 + (*p - '0');
        if (after) s += 1;
    }
    s -= ex;
    if (s < 0) {
        c *= pow10i(-s);
        s = 0;
    }
    if (neg) c = -c;
    return decv_mk(c, s);
}
char* beans_dec_str(BDec* a) {
    __int128 c = a->c;
    int neg = c < 0;
    if (neg) c = -c;
    // scratch sized from the scale: an __int128 holds at most 39 digits, but
    // the zero-fill below runs to scale+1 — "1e-100".to_decimal() legitimately
    // carries scale 100, and the old fixed 64/80-byte stack buffers smashed
    // the stack. Small values keep the stack fast path.
    long long cap = (a->s > 38 ? a->s : 38) + 2;
    char dsmall[64], osmall[80];
    char* digits = cap <= 63 ? dsmall : malloc((size_t)cap + 1);
    char* out = cap + 2 <= 79 ? osmall : malloc((size_t)cap + 3);
    if (!digits || !out) beans_panic("decimal too large to print", 0, 0);
    long long n = 0;
    if (c == 0) digits[n++] = '0';
    while (c > 0) {
        digits[n++] = (char)('0' + (int)(c % 10));
        c /= 10;
    }
    while (n <= a->s) digits[n++] = '0';
    long long o = 0;
    if (neg) out[o++] = '-';
    for (long long i = n - 1; i >= 0; i--) {
        out[o++] = digits[i];
        if (i == a->s && i != 0) out[o++] = '.';
    }
    out[o] = '\0';
    char* r = rc_strdup(out);
    if (digits != dsmall) free(digits);
    if (out != osmall) free(out);
    return r;
}
char* beans_decv_str(BDecV v) {
    BDec d = {decv_coeff(v), decv_scale(v)};
    return beans_dec_str(&d);
}

// ---- std.fmt (mirrors builtins.cpp byte for byte) ----
// same 1e6 width ceiling the interpolation spec enforces at compile time — a
// pad is a fill, not an allocation primitive; past the cap it is a panic on
// both backends, not a 1TB alloc the OOM killer reaps
#define FMT_PAD_MAX 1000000
char* beans_fmt_pad_left(char* s, long long w, long long line, long long col) {
    if (w > FMT_PAD_MAX) beans_panic("pad width too large", line, col);
    long long n = beans_slen(s);
    if (w <= n) return str_make(s, n);
    char* out = beans_alloc(w + 1, w << 3);
    memset(out, ' ', (size_t)(w - n));
    memcpy(out + (w - n), s, (size_t)n);
    return out;
}
char* beans_fmt_pad_right(char* s, long long w, long long line, long long col) {
    if (w > FMT_PAD_MAX) beans_panic("pad width too large", line, col);
    long long n = beans_slen(s);
    if (w <= n) return str_make(s, n);
    char* out = beans_alloc(w + 1, w << 3);
    memcpy(out, s, (size_t)n);
    memset(out + n, ' ', (size_t)(w - n));
    return out;
}
char* beans_fmt_float(double x, long long p) {
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    char buf[512];
    snprintf(buf, sizeof buf, "%.*f", (int)p, x);
    return rc_strdup(buf);
}
char* beans_fmt_dec(BDec* d, long long p) {
    if (p < 0) p = 0;
    if (p > 60) p = 60;
    BDec t = *d;
    if (p < t.s) { // beans_dec_round, on the stack
        __int128 f = pow10i(t.s - p);
        __int128 q = t.c / f, rem = t.c % f;
        if (rem < 0) rem = -rem;
        if (rem * 2 >= f) q += t.c >= 0 ? 1 : -1;
        t.c = q;
        t.s = p;
    }
    char* base = beans_dec_str(&t);
    long long frac = t.s;
    if (p <= frac) return base;
    long long bn = beans_slen(base);
    long long extra = (frac == 0 ? 1 : 0) + (p - frac);
    char* out = beans_alloc(bn + extra + 1, (bn + extra) << 3);
    memcpy(out, base, (size_t)bn);
    long long o = bn;
    if (frac == 0) out[o++] = '.';
    for (long long i = 0; i < p - frac; i++) out[o++] = '0';
    beans_release(base);
    return out;
}
char* beans_decv_fmt(BDecV v, long long p) {
    BDec d = {decv_coeff(v), decv_scale(v)};
    return beans_fmt_dec(&d, p);
}
)RT";
}

} // namespace beans
