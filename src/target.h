#pragma once

#include <cstdint>

#include "types.h"

namespace beans {

struct IntLayout {
    uint8_t bits = 0;
    bool is_signed = false;
};

// Layout facts shared by checking and lowering. Beans currently emits native
// code for the compiler host, so this is the one target selected by `build`.
class TargetLayout {
public:
    enum class Arch { arm64, x86_64, unknown };
    enum class OS { macos, linux, unknown };

    static TargetLayout host() {
        TargetLayout target;
#if defined(__aarch64__) || defined(__arm64__)
        target.arch_ = Arch::arm64;
#elif defined(__x86_64__)
        target.arch_ = Arch::x86_64;
#endif
#if defined(__APPLE__)
        target.os_ = OS::macos;
#elif defined(__linux__)
        target.os_ = OS::linux;
#endif
        return target;
    }

    Arch arch() const { return arch_; }
    OS os() const { return os_; }
    uint8_t pointer_bits() const { return 64; }

    IntLayout integer(Type::K kind) const {
        switch (kind) {
            case Type::K::i8: return {8, true};
            case Type::K::i16: return {16, true};
            case Type::K::i32: return {32, true};
            case Type::K::int_: return {64, true};
            case Type::K::u8: return {8, false};
            case Type::K::u16: return {16, false};
            case Type::K::u32: return {32, false};
            case Type::K::u64_: return {64, false};
            default: return {};
        }
    }

    uint8_t float_bits(Type::K kind) const {
        if (kind == Type::K::f32) return 32;
        if (kind == Type::K::f64_) return 64;
        return 0;
    }

private:
    Arch arch_ = Arch::unknown;
    OS os_ = OS::unknown;
};

} // namespace beans
